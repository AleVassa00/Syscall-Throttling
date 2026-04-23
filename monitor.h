#ifndef MONITOR_H
#define MONITOR_H

#include <linux/types.h>

int monitor_init(void);
void monitor_cleanup(void);

void monitor_enable();
void monitor_disable();
int monitor_should_throttle(void);

int monitor_is_enabled(void);
void monitor_block_current(void);

void monitor_set_max(int max);

#endif