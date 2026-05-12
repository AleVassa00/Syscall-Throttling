#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/kdev_t.h>
#include <linux/errno.h>
#include <linux/rcupdate.h>
#include <linux/wait.h>
#include <linux/kernel.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>

#include "monitor.h"
#include "registry.h"
#include "stats.h"

#define DEFAULT_MAX_CALLS 10
#define WINDOW_MS 1000

static bool monitor_enabled;
static unsigned long max_calls = DEFAULT_MAX_CALLS;

/* Global monitor counter */
static DEFINE_SPINLOCK(counter_lock);
static unsigned long global_count;
static unsigned long window_generation;

/* Wait queue + high-resolution timer */
static DECLARE_WAIT_QUEUE_HEAD(throttle_wq);
static struct hrtimer window_timer;

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

static enum hrtimer_restart window_timer_callback(struct hrtimer *t)
{
    unsigned long flags;

    spin_lock_irqsave(&counter_lock, flags);

    if (READ_ONCE(monitor_enabled)) {
        global_count = 0;
        window_generation++;

        hrtimer_forward_now(&window_timer, ms_to_ktime(WINDOW_MS));

        spin_unlock_irqrestore(&counter_lock, flags);

        wake_up_all(&throttle_wq);

        return HRTIMER_RESTART;
    }

    spin_unlock_irqrestore(&counter_lock, flags);

    wake_up_all(&throttle_wq);

    return HRTIMER_NORESTART;
}


static int try_consume_slot(unsigned long *count_snapshot)
{
    unsigned long flags;
    int allowed = 0;

    spin_lock_irqsave(&counter_lock, flags);

    if (global_count < READ_ONCE(max_calls)) {
        global_count++;
        allowed = 1;
    }

    if (count_snapshot)
        *count_snapshot = global_count;

    spin_unlock_irqrestore(&counter_lock, flags);

    return allowed;
}

void monitor_set_enabled(int val)
{
    unsigned long flags;
    bool enabled = val ? true : false;

    WRITE_ONCE(monitor_enabled, enabled);

    if (enabled) {
          spin_lock_irqsave(&counter_lock, flags);

        global_count = 0;
        window_generation++;

        hrtimer_start(&window_timer,
                      ms_to_ktime(WINDOW_MS),
                      HRTIMER_MODE_REL);

        spin_unlock_irqrestore(&counter_lock, flags);

        stats_on_monitor_start();
    } else {
        
        hrtimer_cancel(&window_timer);
        wake_up_all(&throttle_wq);

        stats_on_monitor_stop();
    }

    printk(KERN_INFO "[MONITOR] %s\n",
           enabled ? "ENABLED" : "DISABLED");
}

void monitor_set_max(unsigned long val)
{
    if (val == 0)
        return;

    WRITE_ONCE(max_calls, val);

    stats_init(); //ATTENZIONE : da rivedere

    printk(KERN_INFO "[MONITOR] MAX set to %lu\n", val);
}

int should_block(int nr)
{
    kuid_t uid = current_euid();
    dev_t dev = 0;
    unsigned long ino = 0;
    int ret;
    int uid_match;
    int prog_match;

    int was_blocked = 0;
    ktime_t block_start = 0;

    if (!READ_ONCE(monitor_enabled))
        return 0;

    if (!is_syscall_monitored(nr))
        return 0;

    ret = get_current_exe_inode(&dev, &ino);
    if (ret) {
        printk(KERN_INFO
               "[MONITOR] get_current_exe_inode failed ret=%d comm=%s\n",
               ret,
               current->comm);
        return 0;
    }

    uid_match = is_uid_monitored(uid);
    prog_match = is_prog_inode_monitored(dev, ino);

    if (!uid_match && !prog_match)
        return 0;

    while (READ_ONCE(monitor_enabled)) {
        unsigned long my_generation;
        unsigned long local_count = 0;
        unsigned long local_max = READ_ONCE(max_calls);
        unsigned long blocked_now = 0;
        unsigned long peak_now = 0;

        if (try_consume_slot(&local_count)) {
            if (was_blocked) {
                ktime_t end = ktime_get();
                s64 delay_ns = ktime_to_ns(ktime_sub(end, block_start));

                stats_on_block_end((u64)delay_ns, uid, current->comm);

                printk(KERN_INFO
                       "[THROTTLE] pid=%d syscall=%d total_waited_ns=%lld total_waited_ms=%lld\n",
                       current->pid,
                       nr,
                       delay_ns,
                       delay_ns / 1000000);
            }

            printk(KERN_INFO
                   "[MONITOR] allowed count=%lu max=%lu euid=%d prog=%s syscall=%d dev=%u:%u ino=%lu\n",
                   local_count,
                   local_max,
                   __kuid_val(uid),
                   current->comm,
                   nr,
                   MAJOR(dev),
                   MINOR(dev),
                   ino);

            return 0;
        }

        if (!was_blocked) {
            was_blocked = 1;
            block_start = ktime_get();

            stats_on_block_start(&blocked_now, &peak_now);

            printk(KERN_INFO
                   "[THROTTLE] pid=%d first_block blocked_now=%lu peak=%lu count=%lu/%lu\n",
                   current->pid,
                   blocked_now,
                   peak_now,
                   local_count,
                   local_max);
        }

        my_generation = READ_ONCE(window_generation);

        ret = wait_event_interruptible(
            throttle_wq,
            READ_ONCE(window_generation) != my_generation ||
            !READ_ONCE(monitor_enabled)
        );

        if (ret) {
            if (was_blocked) {
                ktime_t end = ktime_get();
                s64 delay_ns = ktime_to_ns(ktime_sub(end, block_start));

                stats_on_block_end((u64)delay_ns, uid, current->comm);
            }

            printk(KERN_INFO
                   "[THROTTLE] interrupted by signal pid=%d prog=%s ret=%d\n",
                   current->pid,
                   current->comm,
                   ret);

            return 0;
        }
    }

    if (was_blocked) {
        ktime_t end = ktime_get();
        s64 delay_ns = ktime_to_ns(ktime_sub(end, block_start));

        stats_on_block_end((u64)delay_ns, uid, current->comm);
    }

    return 0;
}

int monitor_init(void)
{
    WRITE_ONCE(monitor_enabled, false);
    WRITE_ONCE(max_calls, DEFAULT_MAX_CALLS);

    global_count = 0;
    window_generation = 0;

    hrtimer_setup(&window_timer,
                  window_timer_callback,
                  CLOCK_MONOTONIC,
                  HRTIMER_MODE_REL);

    stats_init();

    printk(KERN_INFO "[MONITOR] initialized default OFF max=%lu\n",
           READ_ONCE(max_calls));

    return 0;
}


void monitor_cleanup(void)
{
    struct monitor_stats s;

    WRITE_ONCE(monitor_enabled, false);

    hrtimer_cancel(&window_timer);
    wake_up_all(&throttle_wq);

    stats_on_monitor_stop();
    stats_get(&s);

    printk(KERN_INFO "[MONITOR] cleanup\n");

    printk(KERN_INFO
           "[STATS] peak_delay_ns=%llu peak_delay_us=%llu peak_delay_ms=%llu peak_uid=%d peak_prog=%s blocked_total=%lu currently_blocked=%lu peak_blocked=%lu avg_blocked=%lu\n",
           s.peak_delay_ns,
           s.peak_delay_us,
           s.peak_delay_ms,
           s.peak_delay_uid,
           s.peak_delay_comm,
           s.blocked_threads_total,
           s.currently_blocked,
           s.peak_blocked_threads,
           s.avg_blocked_threads);
}