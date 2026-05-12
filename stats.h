#ifndef STATS_H
#define STATS_H

#include <linux/types.h>
#include <linux/cred.h>
#include <linux/sched.h>

struct monitor_stats {
    u64 peak_delay_ns;
    u64 peak_delay_us;
    u64 peak_delay_ms;

    int peak_delay_uid;
    char peak_delay_comm[TASK_COMM_LEN];

    unsigned long blocked_threads_total;
    unsigned long currently_blocked;
    unsigned long peak_blocked_threads;

    u64 blocked_time_sum_ns;
    u64 monitor_time_ns;
    unsigned long avg_blocked_threads;
};

void stats_init(void);
void stats_on_monitor_start(void);
void stats_on_monitor_stop(void);

void stats_on_block_start(unsigned long *blocked_now,
                          unsigned long *peak_now);

void stats_on_block_end(u64 delay_ns, kuid_t uid, const char *comm);

void stats_get(struct monitor_stats *out);

#endif