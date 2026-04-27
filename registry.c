#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/sched.h>

#include "registry.h"

#define MODNAME "syscall_monitor"

// ==============================
// STRUCTURES (SENZA CONTATORI)
// ==============================

struct syscall_entry {
    int nr;
    struct list_head list;
};

struct uid_entry {
    uid_t uid;
    struct list_head list;
};

struct comm_entry {
    char comm[TASK_COMM_LEN];
    struct list_head list;
};

// ==============================
// LIST HEADS
// ==============================
static LIST_HEAD(uid_list);
static LIST_HEAD(comm_list);

// ==============================
// INIT / CLEANUP
// ==============================

int registry_init(void) {
    printk(KERN_INFO "%s: registry initialized\n", MODNAME);
    return 0;
}

void registry_cleanup(void) {

    struct syscall_entry *s, *s_tmp;
    struct uid_entry *u, *u_tmp;
    struct comm_entry *c, *c_tmp;

    list_for_each_entry_safe(s, s_tmp, &syscall_list, list) {
        list_del(&s->list);
        kfree(s);
    }

    list_for_each_entry_safe(u, u_tmp, &uid_list, list) {
        list_del(&u->list);
        kfree(u);
    }

    list_for_each_entry_safe(c, c_tmp, &comm_list, list) {
        list_del(&c->list);
        kfree(c);
    }

    printk(KERN_INFO "%s: registry cleaned\n", MODNAME);
}

// ==============================
// ADD
// ==============================


int add_uid(uid_t uid) {
    struct uid_entry *e;

    list_for_each_entry(e, &uid_list, list) { //check non ottimizzato su presenza nella lista
        if (e->uid == uid)
            return -EEXIST;
    }

    e = kmalloc(sizeof(*e), GFP_KERNEL);
    if (!e)
        return -ENOMEM;

    e->uid = uid;
    list_add(&e->list, &uid_list);
    return 0;
}

int add_comm(const char *comm) {
    struct comm_entry *e;

    list_for_each_entry(e, &comm_list, list) {
        if (strcmp(e->comm, comm) == 0)
            return -EEXIST;
    }

    e = kmalloc(sizeof(*e), GFP_KERNEL);
    if (!e)
        return -ENOMEM;

    strncpy(e->comm, comm, TASK_COMM_LEN);
    e->comm[TASK_COMM_LEN - 1] = '\0';

    list_add(&e->list, &comm_list);
    return 0;
}

// ==============================
// CHECK
// ==============================

int is_uid_monitored(uid_t uid) {
    struct uid_entry *e;

    list_for_each_entry(e, &uid_list, list) {
        if (e->uid == uid)
            return 1;
    }
    return 0;
}

int is_comm_monitored(const char *comm) {
    struct comm_entry *e;

    list_for_each_entry(e, &comm_list, list) {
        if (strcmp(e->comm, comm) == 0)
            return 1;
    }
    return 0;
}

// ==============================
// REMOVE
// ==============================

int remove_uid(uid_t uid) {
    struct uid_entry *e, *tmp;

    list_for_each_entry_safe(e, tmp, &uid_list, list) {
        if (e->uid == uid) {
            list_del(&e->list);
            kfree(e);
            return 0;
        }
    }

    return -ENOENT;
}

int remove_comm(const char *comm) {
    struct comm_entry *e, *tmp;

    list_for_each_entry_safe(e, tmp, &comm_list, list) {
        if (strcmp(e->comm, comm) == 0) {
            list_del(&e->list);
            kfree(e);
            return 0;
        }
    }

    return -ENOENT;
}