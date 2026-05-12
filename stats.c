#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <linux/ktime.h>

#include "stats.h"

static DEFINE_SPINLOCK(stats_lock);

static u64 peak_delay_ns;
static int peak_delay_uid;
static char peak_delay_comm[TASK_COMM_LEN];

static unsigned long blocked_threads_total;
static unsigned long currently_blocked;
static unsigned long peak_blocked_threads;

/*
 * Media corretta:
 * blocked_time_sum_ns = integrale nel tempo di currently_blocked
 * avg = blocked_time_sum_ns / monitor_time_ns
 */
static u64 blocked_time_sum_ns;
static u64 blocking_period_sum_ns;
static ktime_t last_update_time;

static bool monitor_stats_running;
static ktime_t monitor_start_time;
static u64 accumulated_monitor_time_ns;

static void update_blocked_time_locked(void)
{
    ktime_t now;
    u64 delta_ns;

    if (!monitor_stats_running)
        return;

    now = ktime_get();

    delta_ns = ktime_to_ns(ktime_sub(now, last_update_time));

    blocked_time_sum_ns += delta_ns * currently_blocked;

    if (currently_blocked > 0)
        blocking_period_sum_ns += delta_ns;

    last_update_time = now;
}
void stats_init(void)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_lock, flags);

    blocking_period_sum_ns = 0;

    peak_delay_ns = 0;
    peak_delay_uid = -1;
    peak_delay_comm[0] = '\0';

    blocked_threads_total = 0;
    currently_blocked = 0;
    peak_blocked_threads = 0;

    blocked_time_sum_ns = 0;
    last_update_time = ktime_get();

    monitor_stats_running = false;
    monitor_start_time = 0;
    accumulated_monitor_time_ns = 0;

    spin_unlock_irqrestore(&stats_lock, flags);
}

void stats_on_monitor_start(void)
{
    unsigned long flags;
    ktime_t now = ktime_get();

    spin_lock_irqsave(&stats_lock, flags);

    /*
     * Evita doppio start se ENABLE viene chiamato più volte.
     */
    if (!monitor_stats_running) {
        monitor_stats_running = true;
        monitor_start_time = now;
        last_update_time = now;
    }

    spin_unlock_irqrestore(&stats_lock, flags);
}

void stats_on_monitor_stop(void)
{
    unsigned long flags;
    ktime_t now;
    u64 delta_ns;

    spin_lock_irqsave(&stats_lock, flags);

    if (monitor_stats_running) {
        update_blocked_time_locked();

        now = ktime_get();
        delta_ns = ktime_to_ns(ktime_sub(now, monitor_start_time));

        accumulated_monitor_time_ns += delta_ns;
        monitor_stats_running = false;

        /*
         * Quando spegni il monitor, nessuno dovrebbe rimanere logicamente
         * bloccato perché monitor_set_enabled fa wake_up_all().
         */
        currently_blocked = 0;
    }

    spin_unlock_irqrestore(&stats_lock, flags);
}

void stats_on_block_start(unsigned long *blocked_now,
                          unsigned long *peak_now)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_lock, flags);

    update_blocked_time_locked();

    blocked_threads_total++;
    currently_blocked++;

    if (currently_blocked > peak_blocked_threads)
        peak_blocked_threads = currently_blocked;

    if (blocked_now)
        *blocked_now = currently_blocked;

    if (peak_now)
        *peak_now = peak_blocked_threads;

    spin_unlock_irqrestore(&stats_lock, flags);
}

void stats_on_block_end(u64 delay_ns, kuid_t uid, const char *comm)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_lock, flags);

    update_blocked_time_locked();

    if (currently_blocked > 0)
        currently_blocked--;

    if (delay_ns > peak_delay_ns) {
        peak_delay_ns = delay_ns;
        peak_delay_uid = __kuid_val(uid);

        if (comm)
            strscpy(peak_delay_comm, comm, TASK_COMM_LEN);
        else
            peak_delay_comm[0] = '\0';
    }

    spin_unlock_irqrestore(&stats_lock, flags);
}

void stats_get(struct monitor_stats *out)
{
    unsigned long flags;
    u64 total_monitor_time_ns;
    u64 current_run_time_ns = 0;
    ktime_t now;

    if (!out)
        return;

    spin_lock_irqsave(&stats_lock, flags);

    /*
     * Aggiorna l'area temporale fino all'istante della richiesta stats.
     */
    update_blocked_time_locked();

    total_monitor_time_ns = accumulated_monitor_time_ns;

    if (monitor_stats_running) {
        now = ktime_get();
        current_run_time_ns = ktime_to_ns(ktime_sub(now, monitor_start_time));
        total_monitor_time_ns += current_run_time_ns;
    }

    out->peak_delay_ns = peak_delay_ns;
    out->peak_delay_us = peak_delay_ns / 1000;
    out->peak_delay_ms = peak_delay_ns / 1000000;

    out->peak_delay_uid = peak_delay_uid;
    strscpy(out->peak_delay_comm, peak_delay_comm, TASK_COMM_LEN);

    out->blocked_threads_total = blocked_threads_total;
    out->currently_blocked = currently_blocked;
    out->peak_blocked_threads = peak_blocked_threads;

    out->blocked_time_sum_ns = blocked_time_sum_ns;
    out->monitor_time_ns = total_monitor_time_ns;

    if (blocking_period_sum_ns > 0)
        out->avg_blocked_threads =
            div64_u64(blocked_time_sum_ns, blocking_period_sum_ns);
    else
        out->avg_blocked_threads = 0;

    spin_unlock_irqrestore(&stats_lock, flags);
}