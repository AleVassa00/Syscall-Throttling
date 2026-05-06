#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/cred.h>
#include <linux/hashtable.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kdev_t.h>
#include <linux/time.h>

#include "monitor.h"
#include "registry.h"

#define DEFAULT_MAX_CALLS 10
#define MONITOR_HASH_BITS 6

struct counter {
    kuid_t uid;

    dev_t dev;
    unsigned long ino;

    int syscall_nr;

    unsigned long count;
    unsigned long window_start;

    spinlock_t lock;
    struct hlist_node node;
};

static DEFINE_HASHTABLE(counter_table, MONITOR_HASH_BITS);
static DEFINE_MUTEX(table_mutex);

static int monitor_enabled;
static unsigned long max_calls = DEFAULT_MAX_CALLS;

static DEFINE_SPINLOCK(stats_lock);

static unsigned long max_delay;
static unsigned long blocked_threads_total;
static unsigned long currently_blocked;
static unsigned long peak_blocked_threads;

static int get_current_exe_inode(dev_t *dev, unsigned long *ino)
{
    struct inode *inode;

    if (!dev || !ino)
        return -EINVAL;

    /*
     * current->mm->exe_file è il campo diretto, accessibile
     * senza bisogno di funzioni esportate.
     * Protetto da rcu_read_lock come da documentazione kernel.
     */
    rcu_read_lock();

    if (!current->mm || !current->mm->exe_file) {
        rcu_read_unlock();
        return -ENOENT;
    }

    inode = file_inode(current->mm->exe_file);
    if (!inode) {
        rcu_read_unlock();
        return -ENOENT;
    }

    *dev = inode->i_sb->s_dev;
    *ino = inode->i_ino;

    rcu_read_unlock();

    return 0;
}

static u32 counter_hash_key(kuid_t uid, dev_t dev, unsigned long ino, int nr)
{
    u32 key = 0;

    key ^= __kuid_val(uid);
    key ^= MAJOR(dev);
    key ^= MINOR(dev);
    key ^= (u32)ino;
    key ^= (u32)nr;

    return key;
}

static struct counter *find_counter(kuid_t uid,
                                    dev_t dev,
                                    unsigned long ino,
                                    int nr)
{
    struct counter *c;
    u32 key = counter_hash_key(uid, dev, ino, nr);

    hash_for_each_possible(counter_table, c, node, key) {
        if (uid_eq(c->uid, uid) &&
            c->dev == dev &&
            c->ino == ino &&
            c->syscall_nr == nr) {
            return c;
        }
    }

    return NULL;
}

static struct counter *get_or_create_counter(kuid_t uid,
                                             dev_t dev,
                                             unsigned long ino,
                                             int nr)
{
    struct counter *c;
    u32 key = counter_hash_key(uid, dev, ino, nr);

    mutex_lock(&table_mutex);

    c = find_counter(uid, dev, ino, nr);
    if (c) {
        mutex_unlock(&table_mutex);
        return c;
    }

    c = kzalloc(sizeof(*c), GFP_KERNEL);
    if (!c) {
        mutex_unlock(&table_mutex);
        return NULL;
    }

    c->uid = uid;
    c->dev = dev;
    c->ino = ino;
    c->syscall_nr = nr;
    c->count = 0;
    c->window_start = jiffies;

    spin_lock_init(&c->lock);

    hash_add(counter_table, &c->node, key);

    mutex_unlock(&table_mutex);

    return c;
}

void monitor_set_enabled(int val)
{
    monitor_enabled = val ? 1 : 0;

    printk(KERN_INFO "[MONITOR] %s\n",
           monitor_enabled ? "ENABLED" : "DISABLED");
}

void monitor_set_max(unsigned long val)
{
    if (val == 0)
        return;

    max_calls = val;

    printk(KERN_INFO "[MONITOR] MAX set to %lu\n", max_calls);
}

static void stats_on_block_start(void)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_lock, flags);

    blocked_threads_total++;
    currently_blocked++;

    if (currently_blocked > peak_blocked_threads)
        peak_blocked_threads = currently_blocked;

    spin_unlock_irqrestore(&stats_lock, flags);
}

static void stats_on_block_end(unsigned long delay)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_lock, flags);

    if (currently_blocked > 0)
        currently_blocked--;

    if (delay > max_delay)
        max_delay = delay;

    spin_unlock_irqrestore(&stats_lock, flags);
}

int should_block(int nr)
{
    kuid_t uid = current_euid();
    dev_t dev = 0;
    unsigned long ino = 0;
    struct counter *c;
    unsigned long now = jiffies;
    int ret;
    int uid_match;
    int prog_match;

    if (!monitor_enabled)
        return 0;

    if (!is_syscall_monitored(nr))
        return 0;

    ret = get_current_exe_inode(&dev, &ino);
    if (ret) {
        printk(KERN_INFO "[MONITOR] get_current_exe_inode failed ret=%d comm=%s\n",
               ret, current->comm);
        return 0;
    }

    /* DEBUG: stampa sempre cosa vede */
    printk(KERN_INFO "[MONITOR] check: comm=%s dev=%u:%u ino=%lu nr=%d\n",
           current->comm, MAJOR(dev), MINOR(dev), ino, nr);

    uid_match  = is_uid_monitored(uid);
    prog_match = is_prog_inode_monitored(dev, ino);

    printk(KERN_INFO "[MONITOR] uid_match=%d prog_match=%d\n",
           uid_match, prog_match);

    if (!uid_match && !prog_match)
        return 0;

    c = get_or_create_counter(uid, dev, ino, nr);
    if (!c)
        return 0;

    spin_lock(&c->lock);

    now = jiffies;

    if (time_after(now, c->window_start + HZ)) {
        c->count = 0;
        c->window_start = now;
    }

    c->count++;

    printk(KERN_INFO "[MONITOR] euid=%d prog=%s dev=%u:%u ino=%lu syscall=%d count=%lu max=%lu\n",
           __kuid_val(uid),
           current->comm,
           MAJOR(dev),
           MINOR(dev),
           ino,
           nr,
           c->count,
           max_calls);

    if (c->count > max_calls) {
        unsigned long start;
        unsigned long delay;

        spin_unlock(&c->lock);

        printk(KERN_INFO "[THROTTLE] blocking euid=%d prog=%s syscall=%d\n",
               __kuid_val(uid),
               current->comm,
               nr);

        start = jiffies;

        stats_on_block_start();

        set_current_state(TASK_INTERRUPTIBLE);
        schedule_timeout(HZ / 2);

        delay = jiffies - start;

        stats_on_block_end(delay);

        return 1;
    }

    spin_unlock(&c->lock);

    return 0;
}

int monitor_init(void)
{
    hash_init(counter_table);

    monitor_enabled = 0;
    max_calls = DEFAULT_MAX_CALLS;

    max_delay = 0;
    blocked_threads_total = 0;
    currently_blocked = 0;
    peak_blocked_threads = 0;

    printk(KERN_INFO "[MONITOR] initialized default OFF max=%lu\n", max_calls);

    return 0;
}

void monitor_cleanup(void)
{
    int bkt;
    struct counter *c;
    struct hlist_node *tmp;

    mutex_lock(&table_mutex);

    hash_for_each_safe(counter_table, bkt, tmp, c, node) {
        hash_del(&c->node);
        kfree(c);
    }

    mutex_unlock(&table_mutex);

    printk(KERN_INFO "[MONITOR] cleanup\n");
    printk(KERN_INFO "[STATS] max_delay_jiffies=%lu blocked_total=%lu peak_blocked_threads=%lu\n",
           max_delay,
           blocked_threads_total,
           peak_blocked_threads);
}