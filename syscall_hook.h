#ifndef SYSCALL_HOOK_H
#define SYSCALL_HOOK_H

int syscall_hook_init(void);
void syscall_hook_cleanup(void);

int add_syscall_hook(int nr);
int remove_syscall_hook(int nr);

#endif