#ifndef REGISTRY_H
#define REGISTRY_H

#include <linux/types.h>
#include <linux/cred.h>

/* ================= UID ================= */

int add_user_id(int uid);
int remove_user_id(int uid);
int is_uid_monitored(kuid_t uid);

/* ================= PROGRAM INODE ================= */

int add_prog_inode(const char *prog_path);
int remove_prog_inode(const char *prog_path);
int is_prog_inode_monitored(dev_t dev, unsigned long ino);

/* ================= SYSCALL ================= */

int add_syscall(int nr);
int remove_syscall(int nr);
int is_syscall_monitored(int nr);

/* ================= CLEANUP ================= */

void registry_cleanup(void);

#endif /* REGISTRY_H */