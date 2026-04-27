#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ftrace.h>
#include <linux/kallsyms.h>
#include <linux/slab.h>
#include <linux/ptrace.h>
#include <linux/sched.h>

#include "syscall_hook.h"
#include "monitor.h"
#include "registry.h"

#define MODNAME "syscall_monitor"

// ==============================
// GLOBALS
// ==============================

static unsigned long **sys_call_table;

struct ftrace_hook {
    unsigned long address;
    void *function;
    void *original;
    struct ftrace_ops ops;
};

static struct ftrace_hook *hooks;
static int hooks_count;

// ==============================
// FTRACE CALLBACK
// ==============================

static void notrace ftrace_callback(unsigned long ip, unsigned long parent_ip,
                                    struct ftrace_ops *ops, struct pt_regs *regs)
{
    struct ftrace_hook *hook =
        container_of(ops, struct ftrace_hook, ops);

    regs->ip = (unsigned long)hook->function;
}

// ==============================
// INSTALL SINGLE HOOK
// ==============================

static int install_ftrace_hook(struct ftrace_hook *hook)
{
    int ret;

    hook->ops.func = ftrace_callback;
    hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS |
                      FTRACE_OPS_FL_RECURSION_SAFE |
                      FTRACE_OPS_FL_IPMODIFY;

    ret = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
    if (ret) {
        printk(KERN_WARNING "%s: filter failed %d\n", MODNAME, ret);
        return ret;
    }

    ret = register_ftrace_function(&hook->ops);
    if (ret) {
        printk(KERN_WARNING "%s: register failed %d\n", MODNAME, ret);
        ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
        return ret;
    }

    hook->original = (void *)hook->address;

    return 0;
}

// ==============================
// REMOVE SINGLE HOOK
// ==============================

static void remove_ftrace_hook(struct ftrace_hook *hook)
{
    unregister_ftrace_function(&hook->ops);
    ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
}

// ==============================
// WRAPPER
// ==============================

static asmlinkage long syscall_wrapper(const struct pt_regs *regs)
{
    int nr = regs->orig_ax;

    uid_t uid = current_uid().val;
    const char *comm = current->comm;

    if (!monitor_is_enabled())
        return ((long (*)(const struct pt_regs *))hooks[nr].original)(regs);

    if (!is_uid_monitored(uid) && !is_comm_monitored(comm))
        return ((long (*)(const struct pt_regs *))hooks[nr].original)(regs);

    if (monitor_should_throttle())
        monitor_block_current();

    return ((long (*)(const struct pt_regs *))hooks[nr].original)(regs);
}

// ==============================
// INSTALL ALL HOOKS
// ==============================




int install_all_hooks(void)
{
    int i;

    kallsyms_lookup_name_fn = get_kallsyms_lookup_name_fn();

    sys_call_table = (unsigned long **)
        kallsyms_lookup_name_fn("sys_call_table");

    if (!sys_call_table) {
        printk(KERN_ERR "%s: sys_call_table not found\n", MODNAME);
        return -1;
    }

    hooks_count = NR_syscalls;

    hooks = kmalloc_array(hooks_count,
                          sizeof(struct ftrace_hook),
                          GFP_KERNEL);
    if (!hooks)
        return -ENOMEM;

    for (i = 0; i < hooks_count; i++) {

        unsigned long addr = (unsigned long)sys_call_table[i];

        if (!addr)
            continue;

        hooks[i].address = addr;
        hooks[i].function = syscall_wrapper;

        if (install_ftrace_hook(&hooks[i])) {
            printk(KERN_WARNING "%s: hook failed for syscall %d\n",
                   MODNAME, i);
        }
    }

    printk(KERN_INFO "%s: installed %d hooks\n",
           MODNAME, hooks_count);

    return 0;
}

// ==============================
// UNINSTALL ALL HOOKS
// ==============================

void uninstall_all_hooks(void)
{
    int i;

    if (!hooks)
        return;

    for (i = 0; i < hooks_count; i++) {
        if (hooks[i].address)
            remove_ftrace_hook(&hooks[i]);
    }

    kfree(hooks);
    hooks = NULL;

    printk(KERN_INFO "%s: hooks removed\n", MODNAME);
}