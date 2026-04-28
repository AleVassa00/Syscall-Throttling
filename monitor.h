#ifndef MONITOR_H
#define MONITOR_H

int monitor_init(void);
void monitor_cleanup(void);

void monitor_enable(void);
void monitor_disable(void);
int monitor_is_enabled(void);

void monitor_set_max(int max);

int monitor_should_throttle(void);
void monitor_block_current(void);

// STATS
unsigned long long get_peak_delay(void);
int get_peak_blocked(void);
unsigned long long get_avg_blocked(void);

#endif