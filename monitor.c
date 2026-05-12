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
#define WINDOW_MS 1000


static bool monitor_enabled;
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

    if (READ_ONCE(monitor_enabled)) {
        global_count = 0;
        global_window_start = jiffies;
        window_generation++;

        mod_timer(&window_timer,
                  global_window_start + msecs_to_jiffies(WINDOW_MS));
    }

    spin_unlock_irqrestore(&counter_lock, flags);

    wake_up_all(&throttle_wq);
}

//funzione che stabilisce se c'è ancora spazio nella finestra corrente

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
        global_window_start = jiffies;
        window_generation++;

        mod_timer(&window_timer,
                  global_window_start + msecs_to_jiffies(WINDOW_MS));

        spin_unlock_irqrestore(&counter_lock, flags);
    } else {
        timer_delete_sync(&window_timer);
        wake_up_all(&throttle_wq);
    }

    printk(KERN_INFO "[MONITOR] %s\n",
           enabled ? "ENABLED" : "DISABLED");
}


void monitor_set_max(unsigned long val)
{
    if (val == 0)
        return;

    WRITE_ONCE(max_calls, val);

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

    if (!READ_ONCE(monitor_enabled)){
        return 0;
    }

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

    while (READ_ONCE(monitor_enabled)) {
        unsigned long start;
        unsigned long delay;
        unsigned long my_generation;
        unsigned long local_count = 0;
        unsigned long local_max = READ_ONCE(max_calls);

        if (try_consume_slot(&local_count)) {
            printk(KERN_INFO "[MONITOR] allowed count=%lu max=%lu euid=%d prog=%s syscall=%d dev=%u:%u ino=%lu\n",
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

        start = jiffies;


        stats_on_block_start();

        my_generation = window_generation;

        printk(KERN_INFO
       "[THROTTLE] pid=%d blocked_now=%lu peak=%lu count=%lu/%lu\n",
       current->pid,
       currently_blocked,
       peak_blocked_threads,
       local_count,
       local_max);
        ret = wait_event_interruptible(throttle_wq,
                               window_generation != my_generation ||
                               !READ_ONCE(monitor_enabled));
        delay = jiffies - start;
        printk(KERN_INFO
       "[THROTTLE] pid=%d syscall=%d waited_jiffies=%lu waited_ms=%u\n",
       current->pid,
       nr,
       delay,
       jiffies_to_msecs(delay));
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

//inizializzazione monitor e timer della finestra;

int monitor_init(void)
{
    WRITE_ONCE(monitor_enabled, false);
    WRITE_ONCE(max_calls, DEFAULT_MAX_CALLS);

    global_count = 0;
    global_window_start = 0;
    window_generation = 0;

    timer_setup(&window_timer, window_timer_callback, 0);

    max_delay = 0;
    blocked_threads_total = 0;
    currently_blocked = 0;
    peak_blocked_threads = 0;

    printk(KERN_INFO "[MONITOR] initialized default OFF max=%lu\n",
           READ_ONCE(max_calls));

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