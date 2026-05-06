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

#include "syscall_hook.h"
#include "registry.h"

#define MAX_UIDS      64
#define MAX_PROGS     64
#define MAX_SYSCALLS  512

static DEFINE_RWLOCK(registry_lock);

/* ================= UID ================= */

static kuid_t uids[MAX_UIDS];
static int uid_count;

int add_user_id(int uid)
{
    int i;

    write_lock(&registry_lock);

    for (i = 0; i < uid_count; i++) {
        if (__kuid_val(uids[i]) == uid) {
            write_unlock(&registry_lock);
            return -EEXIST;
        }
    }

    if (uid_count >= MAX_UIDS) {
        write_unlock(&registry_lock);
        return -ENOMEM;
    }

    uids[uid_count++] = KUIDT_INIT(uid);

    write_unlock(&registry_lock);

    printk(KERN_INFO "[REGISTRY] added UID %d\n", uid);

    return 0;
}

int remove_user_id(int uid)
{
    int i;

    write_lock(&registry_lock);

    for (i = 0; i < uid_count; i++) {
        if (__kuid_val(uids[i]) == uid) {
            uids[i] = uids[uid_count - 1];
            uid_count--;

            write_unlock(&registry_lock);

            printk(KERN_INFO "[REGISTRY] removed UID %d\n", uid);

            return 0;
        }
    }

    write_unlock(&registry_lock);

    return -ENOENT;
}

int is_uid_monitored(kuid_t uid)
{
    int i;
    int ret = 0;

    read_lock(&registry_lock);

    for (i = 0; i < uid_count; i++) {
        if (uid_eq(uids[i], uid)) {
            ret = 1;
            break;
        }
    }

    read_unlock(&registry_lock);

    return ret;
}

/* ================= PROGRAM INODE ================= */

struct monitored_prog {
    char path[PATH_MAX];

    dev_t dev;
    unsigned long ino;
};

static struct monitored_prog progs[MAX_PROGS];
static int prog_count;

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
    int i;
    int ret;
    dev_t dev;
    unsigned long ino;

    if (!prog_path)
        return -EINVAL;

    ret = resolve_path_inode(prog_path, &dev, &ino);
    if (ret)
        return ret;

    write_lock(&registry_lock);

    for (i = 0; i < prog_count; i++) {
        if (progs[i].dev == dev && progs[i].ino == ino) {
            write_unlock(&registry_lock);
            return -EEXIST;
        }
    }

    if (prog_count >= MAX_PROGS) {
        write_unlock(&registry_lock);
        return -ENOMEM;
    }

    strscpy(progs[prog_count].path, prog_path, PATH_MAX);
    progs[prog_count].dev = dev;
    progs[prog_count].ino = ino;

    prog_count++;

    write_unlock(&registry_lock);

    printk(KERN_INFO "[REGISTRY] added program path=%s dev=%u:%u ino=%lu\n",
           prog_path,
           MAJOR(dev),
           MINOR(dev),
           ino);

    return 0;
}

int remove_prog_inode(const char *prog_path)
{
    int i;
    int ret;
    dev_t dev;
    unsigned long ino;

    if (!prog_path)
        return -EINVAL;

    ret = resolve_path_inode(prog_path, &dev, &ino);
    if (ret)
        return ret;

    write_lock(&registry_lock);

    for (i = 0; i < prog_count; i++) {
        if (progs[i].dev == dev && progs[i].ino == ino) {
            printk(KERN_INFO "[REGISTRY] removed program path=%s dev=%u:%u ino=%lu\n",
                   progs[i].path,
                   MAJOR(progs[i].dev),
                   MINOR(progs[i].dev),
                   progs[i].ino);

            progs[i] = progs[prog_count - 1];
            prog_count--;

            write_unlock(&registry_lock);

            return 0;
        }
    }

    write_unlock(&registry_lock);

    return -ENOENT;
}

int is_prog_inode_monitored(dev_t dev, unsigned long ino)
{
    int i;
    int ret = 0;

    read_lock(&registry_lock);

    for (i = 0; i < prog_count; i++) {
        if (progs[i].dev == dev && progs[i].ino == ino) {
            ret = 1;
            break;
        }
    }

    read_unlock(&registry_lock);

    return ret;
}

/* ================= SYSCALL ================= */

static int syscalls[MAX_SYSCALLS];
static int syscall_count;

int add_syscall(int nr)
{
    int i;

    if (nr < 0 || nr >= MAX_SYSCALLS)
        return -EINVAL;

    write_lock(&registry_lock);

    for (i = 0; i < syscall_count; i++) {
        if (syscalls[i] == nr) {
            write_unlock(&registry_lock);
            return -EEXIST;
        }
    }

    if (syscall_count >= MAX_SYSCALLS) {
        write_unlock(&registry_lock);
        return -ENOMEM;
    }

    syscalls[syscall_count++] = nr;

    write_unlock(&registry_lock);

    printk(KERN_INFO "[REGISTRY] added syscall %d\n", nr);

    return 0;
}

int remove_syscall(int nr)
{
    int i;

    write_lock(&registry_lock);

    for (i = 0; i < syscall_count; i++) {
        if (syscalls[i] == nr) {
            syscalls[i] = syscalls[syscall_count - 1];
            syscall_count--;

            write_unlock(&registry_lock);

            printk(KERN_INFO "[REGISTRY] removed syscall %d\n", nr);

            return 0;
        }
    }

    write_unlock(&registry_lock);

    return -ENOENT;
}

int is_syscall_monitored(int nr)
{
    int i;
    int ret = 0;

    read_lock(&registry_lock);

    for (i = 0; i < syscall_count; i++) {
        if (syscalls[i] == nr) {
            ret = 1;
            break;
        }
    }

    read_unlock(&registry_lock);

    return ret;
}

int registry_add_syscall(int nr)
{
    int ret;

    ret = add_syscall(nr);
    if (ret)
        return ret;

    ret = add_syscall_hook(nr);
    if (ret) {
        remove_syscall(nr);
        return ret;
    }

    return 0;
}

int registry_remove_syscall(int nr)
{
    int ret;

    ret = remove_syscall_hook(nr);
    if (ret)
        return ret;

    return remove_syscall(nr);
}