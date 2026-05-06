#ifndef FHOOK_H
#define FHOOK_H

int add_syscall_hook(int nr);
int remove_syscall_hook(int nr);

int setup_ftrace_hook(void);
void cleanup_ftrace_hook(void);

#endif