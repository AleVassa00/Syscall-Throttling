#ifndef REGISTRY_H
#define REGISTRY_H

#include <linux/types.h>

int registry_init(void);
void registry_cleanup(void);

// check
int is_syscall_monitored(int nr);
int is_uid_monitored(uid_t uid);
int is_comm_monitored(const char *comm);

// add/remove (usati da ioctl)
int add_syscall(int nr);
int remove_syscall(int nr);

int add_uid(uid_t uid);
int remove_uid(uid_t uid);

int add_comm(const char *comm);
int remove_comm(const char *comm);

#endif