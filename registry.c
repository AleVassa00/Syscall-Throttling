#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/cred.h>

#include "registry.h"

#define MODNAME "syscall_monitor"

// ==============================
// STRUCTURES
// ==============================

struct syscall_entry {
    int nr;
    struct list_head list;
};

struct uid_entry {
    uid_t uid;
    atomic_t counter;
    u64 window_start;
    struct list_head list;
};

struct comm_entry {
    char comm[TASK_COMM_LEN];
    atomic_t counter;
    u64 window_start;
    struct list_head list;
};

// ==============================
// LIST HEADS
// ==============================

static LIST_HEAD(syscall_list); //macro che dichiarra variabile globale di tipo list_head
static LIST_HEAD(uid_list);  //idem
static LIST_HEAD(comm_list);  //idem

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
// ADD FUNCTIONS
// ==============================

int add_syscall(int nr) {
    struct syscall_entry *e;

    // evita duplicati
    list_for_each_entry(e, &syscall_list, list) {
        if (e->nr == nr)
            return -EEXIST;
    }

    e = kmalloc(sizeof(*e), GFP_KERNEL); //sto allocando memoria (slab allocator)
    if (!e)
        return -ENOMEM;

    e->nr = nr;
    atomic_set(&e->counter, 0);
    e->window_start = ktime_get_ns();
    list_add(&e->list, &syscall_list);//in testa

    return 0;
}

int add_uid(uid_t uid) {
    struct uid_entry *e;

    list_for_each_entry(e, &uid_list, list) {
        if (e->uid == uid)
            return -EEXIST;
    }

    e = kmalloc(sizeof(*e), GFP_KERNEL);
    if (!e)
        return -ENOMEM;

    e->uid = uid;
    atomic_set(&e->counter, 0);
    e->window_start = ktime_get_ns();    
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
    atomic_set(&e->counter, 0);
    e->window_start = ktime_get_ns();
    list_add(&e->list, &comm_list);

    return 0;
}

// ==============================
// REMOVE FUNCTIONS
// ==============================

int remove_syscall(int nr) {
    struct syscall_entry *e;

    list_for_each_entry(e, &syscall_list, list) {
        if (e->nr == nr) {
            list_del(&e->list);
            kfree(e);
            return 0;
        }
    }

    return -ENOENT;
}

int remove_uid(uid_t uid) {
    struct uid_entry *e;

    list_for_each_entry(e, &uid_list, list) {
        if (e->uid == uid) {
            list_del(&e->list);
            kfree(e);
            return 0;
        }
    }

    return -ENOENT;
}

int remove_comm(const char *comm) {
    struct comm_entry *e;

    list_for_each_entry(e, &comm_list, list) {
        if (strcmp(e->comm, comm) == 0) {
            list_del(&e->list);
            kfree(e);
            return 0;
        }
    }

    return -ENOENT;
}

// ==============================
// CHECK FUNCTIONS (USED IN WRAPPER)
// ==============================

int is_syscall_monitored(int nr) {
    struct syscall_entry *e;

    list_for_each_entry(e, &syscall_list, list) {
        if (e->nr == nr)
            return 1;
    }

    return 0;
}

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

struct uid_entry *find_uid(uid_t uid) {
    struct uid_entry *e;

    list_for_each_entry(e, &uid_list, list) {
        if (e->uid == uid)
            return e;
    }

    return NULL;
}

struct comm_entry *find_comm(const char *comm) {
    struct comm_entry *e;

    list_for_each_entry(e, &comm_list, list) {
        if (strcmp(e->comm, comm) == 0)
            return e;
    }

    return NULL;
}