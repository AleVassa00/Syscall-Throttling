#include <linux/slab.h>
#include <linux/string.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/path.h>
#include <linux/limits.h>
#include <linux/kdev_t.h>
#include <linux/bitmap.h>
#include <linux/hashtable.h>
#include <asm/unistd.h>
#include <linux/rcupdate.h>

#include "registry.h"


static DEFINE_SPINLOCK(registry_path_lock);
static DEFINE_SPINLOCK(registry_uid_lock);

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
    struct rcu_head rcu;
};

struct monitored_prog {
    dev_t dev;
    unsigned long ino;
    char name[NAME_MAX + 1];
    struct hlist_node node;
    struct rcu_head rcu;
};

/* ================ COUNTER ============== */

static int uid_count; 
static int prog_count;

/* ================ HASH_KEY ============= */
/*Calcola la chiave hash associata ad un UID
La chiave viene ricavata dal valore numerico del kuid_t e viene usata per indicizzare la tabella hash degli UID monitorati.
*/
static u32 uid_hash_key(kuid_t uid)
{
    return __kuid_val(uid);
}

/*Calcola la chiave hash associata ad un programma monitorato
 Il programma viene identificato tramite:
 - device del filesystem
 - inode dell'eseguibile
 */
static u32 prog_hash_key(dev_t dev, unsigned long ino)
{
    u32 key = 0;

    key ^= MAJOR(dev);
    key ^= MINOR(dev);
    key ^= (u32)ino;
    key ^= (u32)(ino >> 32);

    return key;
}

/* ================= UID ================= */

/*Aggiunge un UID all'insieme degli utenti monitorati.
 La tabella degli UID usa:
 -spinlock per serializzare gli scrittori
 -RCU per consentire letture lock-free nel path caldo
 La funzione controlla anche duplicati e limite massimo MAX_UIDS.
 */
int add_user_id(int uid)
{
    struct monitored_uid *entry;
    struct monitored_uid *cur;
    kuid_t kuid = KUIDT_INIT(uid);
    u32 key = uid_hash_key(kuid);
    unsigned long flags;

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry){
        return -ENOMEM;
    }

    entry->uid = kuid;

    spin_lock_irqsave(&registry_uid_lock, flags);

    hash_for_each_possible(uid_table, cur, node, key) {
        if (uid_eq(cur->uid, kuid)) {
            spin_unlock_irqrestore(&registry_uid_lock, flags);
            kfree(entry);
            return -EEXIST;
        }
    }

    if (uid_count >= MAX_UIDS) {
        spin_unlock_irqrestore(&registry_uid_lock, flags);
        kfree(entry);
        return -ENOMEM;
    }

    hash_add_rcu(uid_table, &entry->node, key);
    uid_count++;

    spin_unlock_irqrestore(&registry_uid_lock, flags);

    printk(KERN_INFO "[REGISTRY] added UID %d\n", uid);

    return 0;
}
/*Rimuove un UID dall'insieme degli utenti monitorati
 La rimozione avviene tramite hash_del_rcu(), mentre la memoria viene liberata con kfree_rcu() per evitare use-after-free da parte di lettori RCU ancora attivi.
*/
int remove_user_id(int uid)
{
    struct monitored_uid *entry;
    struct hlist_node *tmp;
    kuid_t kuid = KUIDT_INIT(uid);
    u32 key = uid_hash_key(kuid);
    unsigned long flags;

    spin_lock_irqsave(&registry_uid_lock, flags);

    hash_for_each_possible_safe(uid_table, entry, tmp, node, key) {
        if (uid_eq(entry->uid, kuid)) {
            hash_del_rcu(&entry->node);
            uid_count--;

            spin_unlock_irqrestore(&registry_uid_lock, flags);

            kfree_rcu(entry, rcu);

            printk(KERN_INFO "[REGISTRY] removed UID %d\n", uid);

            return 0;
        }
    }

    spin_unlock_irqrestore(&registry_uid_lock, flags);

    return -ENOENT;
}

/*Verifica se l'effective UID corrente è monitorato
 Questa funzione viene invocata nel path caldo del monitor, quindi usa RCU per evitare lock pesanti durante le syscall.
*/
int is_uid_monitored(kuid_t uid){
    struct monitored_uid *entry;
    u32 key = uid_hash_key(uid);
    int ret = 0;

    rcu_read_lock();

    hash_for_each_possible_rcu(uid_table, entry, node, key) {
        if (uid_eq(entry->uid, uid)) {
            ret = 1;
            break;
        }
    }

    rcu_read_unlock();

    return ret;
}


/* ================= PROGRAM INODE ================= */

/*Risolve il path di un eseguibile nella coppia device/inode.
 Internamente il registry non confronta il nome del programma, ma l'identità reale del file eseguibile:
 - dev: device del filesystem
 - ino: inode del file
 */

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

/*Aggiunge un programma all'insieme dei programmi monitorati
 - Il path ricevuto viene risolto in device/inode e solo questa coppia viene usata per il matching runtime. Il nome viene salvato solo come informazione ausiliaria/debug.
*/

int add_prog_inode(const char *prog_path)
{
    int ret;
    dev_t dev;
    unsigned long ino;
    struct monitored_prog *entry;
    struct monitored_prog *cur;
    u32 key;
    unsigned long flags;

    if (!prog_path)
        return -EINVAL;

    ret = resolve_path_inode(prog_path, &dev, &ino);
    if (ret)
        return ret;

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return -ENOMEM;

    entry->dev = dev;
    entry->ino = ino;
    strscpy(entry->name, kbasename(prog_path), sizeof(entry->name));

    key = prog_hash_key(dev, ino);

    spin_lock_irqsave(&registry_path_lock, flags);

    hash_for_each_possible(progs_table, cur, node, key) {
        if (cur->dev == dev && cur->ino == ino) {
            spin_unlock_irqrestore(&registry_path_lock, flags);
            kfree(entry);
            return -EEXIST;
        }
    }

    if (prog_count >= MAX_PROGS) {
        spin_unlock_irqrestore(&registry_path_lock, flags);
        kfree(entry);
        return -ENOMEM;
    }

    hash_add_rcu(progs_table, &entry->node, key);
    prog_count++;

    spin_unlock_irqrestore(&registry_path_lock, flags);

    printk(KERN_INFO "[REGISTRY] added program dev=%u:%u ino=%lu\n",
           MAJOR(dev), MINOR(dev), ino);

    return 0;
}

/*Rimuove un programma dall'insieme dei programmi monitorati.
 Come per gli UID, la rimozione usa RCU:
 - hash_del_rcu() scollega il nodo dalla tabella
 - kfree_rcu() libera la memoria solo dopo il grace period
*/

int remove_prog_inode(const char *prog_path)
{
    int ret;
    dev_t dev;
    unsigned long ino;
    struct monitored_prog *entry;
    struct hlist_node *tmp;
    u32 key;
    unsigned long flags;

    if (!prog_path)
        return -EINVAL;

    ret = resolve_path_inode(prog_path, &dev, &ino);
    if (ret)
        return ret;

    key = prog_hash_key(dev, ino);

    spin_lock_irqsave(&registry_path_lock, flags);

    hash_for_each_possible_safe(progs_table, entry, tmp, node, key) {
        if (entry->dev == dev && entry->ino == ino) {
            printk(KERN_INFO "[REGISTRY] removed program dev=%u:%u ino=%lu\n",
                   MAJOR(entry->dev),
                   MINOR(entry->dev),
                   entry->ino);

            hash_del_rcu(&entry->node);
            prog_count--;

            spin_unlock_irqrestore(&registry_path_lock, flags);

            kfree_rcu(entry, rcu);

            return 0;
        }
    }

    spin_unlock_irqrestore(&registry_path_lock, flags);

    return -ENOENT;
}

/*Verifica se il programma corrente è monitorato tramite device/inode.
 È una funzione del path caldo: viene chiamata durante il controllo delle syscall, quindi usa RCU per minimizzare l'overhead.
*/

int is_prog_inode_monitored(dev_t dev, unsigned long ino)
{
    struct monitored_prog *entry;
    u32 key = prog_hash_key(dev, ino);
    int ret = 0;

    rcu_read_lock();

    hash_for_each_possible_rcu(progs_table, entry, node, key) {
        if (entry->dev == dev && entry->ino == ino) {
            ret = 1;
            break;
        }
    }

    rcu_read_unlock();

    return ret;
}

/* ================= SYSCALL ================= */

/*Aggiunge una syscall alla bitmap delle syscall monitorate.
 test_and_set_bit() esegue atomicamente:
 - lettura del valore precedente
 - set del bit
In questo modo non serve un lock esplicito per evitare race tra due ioctl concorrenti che aggiungono la stessa syscall.
*/
int add_syscall(int nr)
{
    if (nr < 0 || nr >= MAX_SYSCALLS)
        return -EINVAL;

    if (test_and_set_bit(nr, monitored_syscalls))
        return -EEXIST;

    printk(KERN_INFO "[REGISTRY] added syscall %d\n", nr);

    return 0;
}

/*Rimuove una syscall dalla bitmap delle syscall monitorate.
 test_and_clear_bit() esegue atomicamente:
 - lettura del valore precedente
 - clear del bit
 Se il bit era già a 0, la syscall non era registrata.
*/
int remove_syscall(int nr)
{
    if (nr < 0 || nr >= MAX_SYSCALLS)
        return -EINVAL;

    if (!test_and_clear_bit(nr, monitored_syscalls))
        return -ENOENT;

    printk(KERN_INFO "[REGISTRY] removed syscall %d\n", nr);

    return 0;
}

/*Verifica se una syscall è monitorata
 La lettura della bitmap è lock-free tramite test_bit(), così il controllo resta molto veloce nel path caldo del monitor.
*/
int is_syscall_monitored(int nr)
{
    if (nr < 0 || nr >= MAX_SYSCALLS)
        return 0;

    return test_bit(nr, monitored_syscalls);
}
 
/*Libera tutte le strutture dinamiche del registry
 UID e programmi sono rimossi con primitive RCU:
 - hash_del_rcu()
 - kfree_rcu()
 synchronize_rcu() assicura che eventuali lettori RCU ancora attivi abbiano terminato prima della conclusione della cleanup.
*/
void registry_cleanup(void)
{
    int bkt;
    struct monitored_uid *u;
    struct monitored_prog *p;
    struct hlist_node *tmp;
    unsigned long uid_flags;
    unsigned long prog_flags;

    spin_lock_irqsave(&registry_uid_lock, uid_flags);

    hash_for_each_safe(uid_table, bkt, tmp, u, node) {
        hash_del_rcu(&u->node);
        kfree_rcu(u, rcu);
    }

    uid_count = 0;

    spin_unlock_irqrestore(&registry_uid_lock, uid_flags);

    spin_lock_irqsave(&registry_path_lock, prog_flags);

    hash_for_each_safe(progs_table, bkt, tmp, p, node) {
        hash_del_rcu(&p->node);
        kfree_rcu(p, rcu);
    }

    prog_count = 0;

    spin_unlock_irqrestore(&registry_path_lock, prog_flags);

    synchronize_rcu();
}

/*Copia nella struttura di output la lista degli UID monitorati.
 La lettura della tabella avviene tramite RCU:
 - non blocca eventuali lettori nel path caldo
 - consente di attraversare la hash table in modo sicuro anche se un writer rimuove un UID in parallelo
 La funzione restituisce uno snapshot della configurazione corrente.
*/

int get_uid_list(struct uid_list *out)
{
    struct monitored_uid *entry;
    int bkt;
    int i = 0;

    if (!out)
        return -EINVAL;

    rcu_read_lock();

    hash_for_each_rcu(uid_table, bkt, entry, node) {
        if (i >= MAX_UIDS)
            break;

        out->uids[i++] = __kuid_val(entry->uid);
    }

    out->count = i;

    rcu_read_unlock();

    return 0;
}

/*Copia in una struttura di output la lista dei programmi monitorati.
 I programmi sono esportati tramite coppia:
 - major/minor del device
 - inode dell'eseguibile
 Questa rappresentazione evita dipendenza dal path testuale,che può cambiare nel filesystem.
*/
int get_prog_list(struct prog_list *out)
{
    struct monitored_prog *entry;
    int bkt;
    int i = 0;

    if (!out)
        return -EINVAL;

    rcu_read_lock();

    hash_for_each_rcu(progs_table, bkt, entry, node) {
        if (i >= MAX_PROGS)
            break;

        out->entries[i].major = MAJOR(entry->dev);
        out->entries[i].minor = MINOR(entry->dev);
        out->entries[i].ino   = entry->ino;
        i++;
    }

    out->count = i;

    rcu_read_unlock();

    return 0;
}

/*Copia in userspace la lista delle syscall monitorate.
 La funzione scorre la bitmap e costruisce uno snapshot della configurazione corrente. Lo snapshot può non essere perfettamente atomico rispetto ad add/remove concorrenti, ma è sufficiente per una ioctl di sola lettura.
*/
int get_syscall_list(struct syscall_list *out)
{
    int nr;
    int i = 0;

    if (!out)
        return -EINVAL;

    for (nr = 0; nr < MAX_SYSCALLS; nr++) {
        if (test_bit(nr, monitored_syscalls)) {
            if (i >= MAX_SYSCALLS)
                break;

            out->nrs[i++] = nr;
        }
    }

    out->count = i;

    return 0;
}