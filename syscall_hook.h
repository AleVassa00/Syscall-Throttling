#ifndef SYSCALL_HOOK_H
#define SYSCALL_HOOK_H

#include <linux/types.h>

// syscall table
extern unsigned long **sys_call_table;

// init
int resolve_syscall_table(void);

// hook management
int install_syscall_hooks(void);
void uninstall_syscall_hooks(void);

#endif