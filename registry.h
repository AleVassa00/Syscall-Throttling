#ifndef REGISTRY_H
#define REGISTRY_H

#include <linux/types.h>
#include <linux/cred.h>

/* ================= USERSPACE LIST =============== */
#define MAX_UIDS     64
#define MAX_PROGS    64
#define MAX_SYSCALLS 512

/* struct per trasferire le liste in userspace */
struct uid_list {
    int count;
    int uids[MAX_UIDS];
};

struct prog_list {
    int count;
    struct prog_entry {
        unsigned int major;
        unsigned int minor;
        unsigned long ino;
    } entries[MAX_PROGS];
};

struct syscall_list {
    int count;
    int nrs[MAX_SYSCALLS];
};

int get_uid_list(struct uid_list *out);
int get_prog_list(struct prog_list *out);
int get_syscall_list(struct syscall_list *out);

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