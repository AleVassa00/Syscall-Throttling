#ifndef REGISTRY_H
#define REGISTRY_H

#include <linux/types.h>

int registry_init(void);
void registry_cleanup(void);

// ADD
int add_uid(uid_t uid);
int add_comm(const char *comm);
int add_syscall(int nr);

// REMOVE
int remove_uid(uid_t uid);
int remove_comm(const char *comm);
int remove_syscall(int nr);

// CHECK
int is_uid_monitored(uid_t uid);
int is_comm_monitored(const char *comm);
int is_syscall_monitored(int nr);

// EMPTY CHECK
int is_uid_list_empty(void);
int is_comm_list_empty(void);
int is_syscall_list_empty(void);

#endif