#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kdev_t.h>
#include <linux/time.h>
#include <linux/errno.h>
#include <linux/rcupdate.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/timer.h>

#include "monitor.h"
#include "registry.h"

#define DEFAULT_MAX_CALLS 10
#define WINDOW_MS 10000


static int monitor_enabled;
static unsigned long max_calls = DEFAULT_MAX_CALLS;

/* Contatore globale del monitor */
static DEFINE_SPINLOCK(counter_lock);
static unsigned long global_count;
static unsigned long global_window_start;
static unsigned long window_generation;

/* Wait queue + timer finestra */
static DECLARE_WAIT_QUEUE_HEAD(throttle_wq);
static struct timer_list window_timer;

/* Statistiche */
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

static void window_timer_callback(struct timer_list *t)
{
    unsigned long flags;

    spin_lock_irqsave(&counter_lock, flags);

    global_count = 0;
    global_window_start = jiffies;
    window_generation++;

    spin_unlock_irqrestore(&counter_lock, flags);

    wake_up_all(&throttle_wq);
}

static int try_consume_slot(unsigned long *count_snapshot)
{
    unsigned long flags;
    unsigned long now;
    int allowed = 0;
    int wake = 0;

    spin_lock_irqsave(&counter_lock, flags);

    now = jiffies;

    if (time_after_eq(now, global_window_start + msecs_to_jiffies(WINDOW_MS))) {
        global_count = 0;
        global_window_start = now;
        window_generation++;
        wake = 1;
    }

    if (global_count < max_calls) {
        global_count++;
        allowed = 1;
    } else {
        mod_timer(&window_timer, global_window_start + msecs_to_jiffies(WINDOW_MS));
    }

    if (count_snapshot)
        *count_snapshot = global_count;

    spin_unlock_irqrestore(&counter_lock, flags);

    if (wake)
        wake_up_all(&throttle_wq);

    return allowed;
}

void monitor_set_enabled(int val)
{
    monitor_enabled = val ? 1 : 0;

    if (!monitor_enabled)
        wake_up_all(&throttle_wq);

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

int should_block(int nr)
{
    kuid_t uid = current_euid();
    dev_t dev = 0;
    unsigned long ino = 0;
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

    uid_match = is_uid_monitored(uid);
    prog_match = is_prog_inode_monitored(dev, ino);

    if (!uid_match && !prog_match)
        return 0;

    while (monitor_enabled) {
        unsigned long start;
        unsigned long delay;
        unsigned long my_generation;
        unsigned long local_count = 0;

        if (try_consume_slot(&local_count)) {
            printk(KERN_INFO "[MONITOR] allowed count=%lu max=%lu euid=%d prog=%s syscall=%d dev=%u:%u ino=%lu\n",
                   local_count,
                   max_calls,
                   __kuid_val(uid),
                   current->comm,
                   nr,
                   MAJOR(dev),
                   MINOR(dev),
                   ino);

            return 0;
        }

        start = jiffies;

        stats_on_block_start();

        my_generation = window_generation;

        printk(KERN_INFO
       "[THROTTLE] pid=%d blocked_now=%lu peak=%lu count=%lu/%lu\n",
       current->pid,
       currently_blocked,
       peak_blocked_threads,
       local_count,
       max_calls);
        ret = wait_event_interruptible(throttle_wq,
                               window_generation != my_generation ||
                               !monitor_enabled);

        delay = jiffies - start;

        stats_on_block_end(delay);

        if (ret) {
        printk(KERN_INFO
           "[THROTTLE] interrupted by signal pid=%d prog=%s ret=%d\n",
           current->pid,
           current->comm,
           ret);
        return 0;
        }
    }

    return 0;
}

int monitor_init(void)
{
    monitor_enabled = 0;
    max_calls = DEFAULT_MAX_CALLS;

    global_count = 0;
    global_window_start = jiffies;
    window_generation = 0;

    timer_setup(&window_timer, window_timer_callback, 0);

    max_delay = 0;
    blocked_threads_total = 0;
    currently_blocked = 0;
    peak_blocked_threads = 0;

    printk(KERN_INFO "[MONITOR] initialized default OFF max=%lu\n", max_calls);

    return 0;
}

void monitor_cleanup(void)
{
    timer_delete_sync(&window_timer);
    wake_up_all(&throttle_wq);

    printk(KERN_INFO "[MONITOR] cleanup\n");

    printk(KERN_INFO "[STATS] max_delay_jiffies=%lu blocked_total=%lu peak_blocked_threads=%lu\n",
           max_delay,
           blocked_threads_total,
           peak_blocked_threads);
}