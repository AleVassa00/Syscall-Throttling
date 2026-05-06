#ifndef MONITOR_H
#define MONITOR_H

#include <linux/types.h>

int monitor_init(void);
void monitor_cleanup(void);

void monitor_set_enabled(int val);
void monitor_set_max(unsigned long val);

int should_block(int nr);

#endif