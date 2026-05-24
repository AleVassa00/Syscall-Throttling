#ifndef MONITOR_H
#define MONITOR_H

#include <linux/types.h>

int monitor_init(void);
void monitor_cleanup(void);

void monitor_enable(void);
void monitor_disable(void);
void monitor_set_max(unsigned long val);
void monitor_wake_throttled(void);

                               

int should_block(int nr);

#endif