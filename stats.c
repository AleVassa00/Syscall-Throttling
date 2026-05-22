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
 * blocked_time_sum_ns:
 *   integrale temporale di currently_blocked.
 *
 * blocking_period_sum_ns:
 *   tempo totale in cui almeno un thread era bloccato.
 *
 * avg_blocked_threads_x1000:
 *   blocked_time_sum_ns / blocking_period_sum_ns,
 *   scalato x1000 per mantenere tre cifre decimali.
 */
static u64 blocked_time_sum_ns;
static u64 blocking_period_sum_ns;
static u64 total_monitor_time_ns;
static ktime_t last_update_time;

static bool monitor_stats_running;
static ktime_t monitor_start_time;

/*
 * Aggiorna l'integrale temporale dei thread bloccati.
 *
 * Questa funzione deve essere chiamata sotto stats_lock.
 */
static void update_blocked_time_locked(void)
{
    ktime_t now;
    u64 delta_ns;

    if (!monitor_stats_running)
        return;

    now = ktime_get();
    delta_ns = ktime_to_ns(ktime_sub(now, last_update_time));

    total_monitor_time_ns += delta_ns;

    blocked_time_sum_ns += delta_ns * currently_blocked;

    if (currently_blocked > 0)
        blocking_period_sum_ns += delta_ns;

    last_update_time = now;
}

/*
 * Reset interno delle statistiche.
 *
 * Se monitor_is_running è true, le statistiche ripartono
 * immediatamente da una nuova origine temporale.
 */
static void stats_reset_locked(bool monitor_is_running)
{
    ktime_t now = ktime_get();

    peak_delay_ns = 0;
    peak_delay_uid = -1;
    peak_delay_comm[0] = '\0';

    blocked_threads_total = 0;
    currently_blocked = 0;
    peak_blocked_threads = 0;

    total_monitor_time_ns = 0;
    blocked_time_sum_ns = 0;
    blocking_period_sum_ns = 0;

    last_update_time = now;

    monitor_stats_running = monitor_is_running;
    monitor_start_time = monitor_is_running ? now : 0;
}

void stats_init(void)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_lock, flags);
    stats_reset_locked(false);
    spin_unlock_irqrestore(&stats_lock, flags);
}

void stats_reset(bool monitor_is_running)
{
    unsigned long flags;

    spin_lock_irqsave(&stats_lock, flags);
    stats_reset_locked(monitor_is_running);
    spin_unlock_irqrestore(&stats_lock, flags);
}

void stats_on_monitor_start(void)
{
    unsigned long flags;
    ktime_t now = ktime_get();

    spin_lock_irqsave(&stats_lock, flags);

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

    spin_lock_irqsave(&stats_lock, flags);

    if (monitor_stats_running) {
        update_blocked_time_locked();   // ← aggiorna total_monitor_time_ns
        monitor_stats_running = false;
        currently_blocked = 0;
    }

    spin_unlock_irqrestore(&stats_lock, flags);
}

void stats_on_block_start(unsigned long *blocked_now,unsigned long *peak_now)
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

    if (!out)
        return;

    spin_lock_irqsave(&stats_lock, flags);

    update_blocked_time_locked();
    

    out->peak_delay_ns = peak_delay_ns;
    out->peak_delay_us = peak_delay_ns / 1000;
    out->peak_delay_ms = peak_delay_ns / 1000000;

    out->peak_delay_uid = peak_delay_uid;
    strscpy(out->peak_delay_comm, peak_delay_comm, TASK_COMM_LEN);
    out->blocked_threads_total = blocked_threads_total;
    out->peak_blocked_threads = peak_blocked_threads;

    if (total_monitor_time_ns > 0) {
        out->avg_blocked_threads_global_x1000 =
            div64_u64(blocked_time_sum_ns * 1000,
                      total_monitor_time_ns);
    } else {
        out->avg_blocked_threads_global_x1000 = 0;
    }

    if (blocking_period_sum_ns > 0) {
        out->avg_blocked_threads_during_throttle_x1000 =
            div64_u64(blocked_time_sum_ns * 1000,
                      blocking_period_sum_ns);
    } else {
        out->avg_blocked_threads_during_throttle_x1000 = 0;
    }

    spin_unlock_irqrestore(&stats_lock, flags);
}