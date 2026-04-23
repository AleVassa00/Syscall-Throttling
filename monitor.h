#ifndef MONITOR_H
#define MONITOR_H

#include <linux/types.h>

int monitor_init(void);
void monitor_cleanup(void);

void monitor_set_enabled(int val);
int monitor_should_throttle(int syscall_nr);
void monitor_block_current(void);

void monitor_set_max(int max);

#endif