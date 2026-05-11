#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/limits.h>
#include <linux/kdev_t.h>
#include <linux/rwlock.h>
#include <linux/bitmap.h>
#include <linux/hashtable.h>
#include <linux/jhash.h>
#include <asm/unistd.h>

#include "registry.h"

#define MAX_UIDS      64
#define MAX_PROGS     64
#define MAX_SYSCALLS  __NR_syscalls

static DEFINE_RWLOCK(registry_syscall_lock);
static DEFINE_RWLOCK(registry_path_lock);
static DEFINE_RWLOCK(registry_uid_lock);


 /* =============== REGISTRY ============= */

static DECLARE_BITMAP(monitored_syscalls, MAX_SYSCALLS);

#define UID_HASH_BITS   4
#define PROG_HASH_BITS  5

static DEFINE_HASHTABLE(progs_table, PROG_HASH_BITS);
static DEFINE_HASHTABLE(uid_table, UID_HASH_BITS);


/* ================ STRUCT =============== */

struct monitored_uid {
    kuid_t uid;
    struct hlist_node node;
};

struct monitored_prog {
    dev_t dev;
    unsigned long ino;
    struct hlist_node node;
};

/* ================ COUNTER ============== */

static int uid_count; 
static int prog_count;

/* ================ HASH_KEY ============= */

static u32 uid_hash_key(kuid_t uid)
{
    return __kuid_val(uid);
}

static u32 prog_hash_key(dev_t dev, unsigned long ino)
{
    u32 key = 0;

    key ^= MAJOR(dev);
    key ^= MINOR(dev);
    key ^= (u32)ino;
    key ^= (u32)(ino >> 32);

    return key;
}


static struct monitored_uid *find_uid_nolock(kuid_t uid)
{
    struct monitored_uid *entry;
    u32 key = uid_hash_key(uid);

    hash_for_each_possible(uid_table, entry, node, key) {
        if (uid_eq(entry->uid, uid))
            return entry;
    }

    return NULL;
}

static struct monitored_prog *find_prog_nolock(dev_t dev, unsigned long ino)
{
    struct monitored_prog *entry;
    u32 key = prog_hash_key(dev, ino);

    hash_for_each_possible(progs_table, entry, node, key) {
        if (entry->dev == dev && entry->ino == ino)
            return entry;
    }

    return NULL;
}

/* ================= UID ================= */


int add_user_id(int uid)
{
    struct monitored_uid *entry;
    kuid_t kuid = KUIDT_INIT(uid);
    u32 key;

    write_lock(&registry_uid_lock);

    if (find_uid_nolock(kuid)) {
        write_unlock(&registry_uid_lock);
        return -EEXIST;
    }

    if (uid_count >= MAX_UIDS) {
        write_unlock(&registry_uid_lock);
        return -ENOMEM;
    }

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        write_unlock(&registry_uid_lock);
        return -ENOMEM;
    }

    entry->uid = kuid;
    key = uid_hash_key(kuid);

    hash_add(uid_table, &entry->node, key);
    uid_count++;

    write_unlock(&registry_uid_lock);

    printk(KERN_INFO "[REGISTRY] added UID %d\n", uid);

    return 0;
}

int remove_user_id(int uid)
{
    struct monitored_uid *entry;
    struct hlist_node *tmp;
    kuid_t kuid = KUIDT_INIT(uid);
    u32 key = uid_hash_key(kuid);

    write_lock(&registry_uid_lock);

    hash_for_each_possible_safe(uid_table, entry, tmp, node, key) {
        if (uid_eq(entry->uid, kuid)) {
            hash_del(&entry->node);
            kfree(entry);
            uid_count--;

            write_unlock(&registry_uid_lock);

            printk(KERN_INFO "[REGISTRY] removed UID %d\n", uid);

            return 0;
        }
    }

    write_unlock(&registry_uid_lock);

    return -ENOENT;
}

int is_uid_monitored(kuid_t uid)
{
    int ret;

    read_lock(&registry_uid_lock);

    ret = find_uid_nolock(uid) ? 1 : 0;

    read_unlock(&registry_uid_lock);

    return ret;
}


/* ================= PROGRAM INODE ================= */

static int resolve_path_inode(const char *prog_path, dev_t *dev, unsigned long *ino)
{
    struct path path;
    struct inode *inode;
    int ret;

    if (!prog_path || !dev || !ino)
        return -EINVAL;

    ret = kern_path(prog_path, LOOKUP_FOLLOW, &path);
    if (ret) {
        printk(KERN_ERR "[REGISTRY] kern_path failed path=%s ret=%d\n",
               prog_path, ret);
        return ret;
    }

    inode = d_inode(path.dentry);
    if (!inode) {
        path_put(&path);
        return -ENOENT;
    }

    *dev = inode->i_sb->s_dev;
    *ino = inode->i_ino;

    path_put(&path);

    return 0;
}



int add_prog_inode(const char *prog_path)
{
    int ret;
    dev_t dev;
    unsigned long ino;
    struct monitored_prog *entry;
    u32 key;

    if (!prog_path)
        return -EINVAL;

    ret = resolve_path_inode(prog_path, &dev, &ino);
    if (ret)
        return ret;

    write_lock(&registry_path_lock);

    if (find_prog_nolock(dev, ino)) {
        write_unlock(&registry_path_lock);
        return -EEXIST;
    }

    if (prog_count >= MAX_PROGS) {
        write_unlock(&registry_path_lock);
        return -ENOMEM;
    }

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry) {
        write_unlock(&registry_path_lock);
        return -ENOMEM;
    }

    entry->dev = dev;
    entry->ino = ino;
    key = prog_hash_key(dev, ino);

    hash_add(progs_table, &entry->node, key);
    prog_count++;

    write_unlock(&registry_path_lock);

    printk(KERN_INFO "[REGISTRY] added program dev=%u:%u ino=%lu\n",
           MAJOR(dev), MINOR(dev), ino);

    return 0;
}

int remove_prog_inode(const char *prog_path)
{
    int ret;
    dev_t dev;
    unsigned long ino;
    struct monitored_prog *entry;
    struct hlist_node *tmp;
    u32 key;

    if (!prog_path)
        return -EINVAL;

    ret = resolve_path_inode(prog_path, &dev, &ino);
    if (ret)
        return ret;

    key = prog_hash_key(dev, ino);

    write_lock(&registry_path_lock);

    hash_for_each_possible_safe(progs_table, entry, tmp, node, key) {
        if (entry->dev == dev && entry->ino == ino) {
            printk(KERN_INFO "[REGISTRY] removed program dev=%u:%u ino=%lu\n",
                   MAJOR(entry->dev), MINOR(entry->dev), entry->ino);

            hash_del(&entry->node);
            kfree(entry);
            prog_count--;

            write_unlock(&registry_path_lock);

            return 0;
        }
    }

    write_unlock(&registry_path_lock);

    return -ENOENT;
}

int is_prog_inode_monitored(dev_t dev, unsigned long ino)
{
    int ret;

    read_lock(&registry_path_lock);

    ret = find_prog_nolock(dev, ino) ? 1 : 0;

    read_unlock(&registry_path_lock);

    return ret;
}

/* ================= SYSCALL ================= */


int add_syscall(int nr)
{
    if (nr < 0 || nr >= MAX_SYSCALLS)
        return -EINVAL;

    write_lock(&registry_syscall_lock);

    if (test_bit(nr, monitored_syscalls)) {
        write_unlock(&registry_syscall_lock);
        return -EEXIST;
    }

    set_bit(nr, monitored_syscalls);

    write_unlock(&registry_syscall_lock);

    printk(KERN_INFO "[REGISTRY] added syscall %d\n", nr);

    return 0;
}

int remove_syscall(int nr)
{
    if (nr < 0 || nr >= MAX_SYSCALLS)
        return -EINVAL;

    write_lock(&registry_syscall_lock);

    if (!test_bit(nr, monitored_syscalls)) {
        write_unlock(&registry_syscall_lock);
        return -ENOENT;
    }

    clear_bit(nr, monitored_syscalls);

    write_unlock(&registry_syscall_lock);

    printk(KERN_INFO "[REGISTRY] removed syscall %d\n", nr);

    return 0;
}

int is_syscall_monitored(int nr)
{
    int ret;

    if (nr < 0 || nr >= MAX_SYSCALLS)
        return 0;

    read_lock(&registry_syscall_lock);

    ret = test_bit(nr, monitored_syscalls);

    read_unlock(&registry_syscall_lock);

    return ret;
}
void registry_cleanup(void)
{
    int bkt;
    struct monitored_uid *u;
    struct monitored_prog *p;
    struct hlist_node *tmp;

    write_lock(&registry_uid_lock);

    hash_for_each_safe(uid_table, bkt, tmp, u, node) {
        hash_del(&u->node);
        kfree(u);
    }

    uid_count = 0;

    write_unlock(&registry_uid_lock);

    write_lock(&registry_path_lock);

    hash_for_each_safe(progs_table, bkt, tmp, p, node) {
        hash_del(&p->node);
        kfree(p);
    }

    prog_count = 0;

    write_unlock(&registry_path_lock);
}