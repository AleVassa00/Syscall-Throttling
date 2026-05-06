#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ftrace.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/string.h>

#include "fhook.h"
#include "probe.h"
#include "monitor.h"
#include "syscall_table.h"

struct syscall_hook {
    int nr;
    char name[64];
    unsigned long addr;
    struct ftrace_ops ops;
    bool active;
};

#define MAX_HOOKS 64

static struct syscall_hook *hooks[MAX_HOOKS];
static int hook_count = 0;

static struct syscall_hook *find_hook(int nr)
{
    int i;

    for (i = 0; i < hook_count; i++) {
        if (hooks[i] && hooks[i]->nr == nr)
            return hooks[i];
    }

    return NULL;
}

static int syscall_symbol_from_nr(int nr, char *buf, size_t size)
{
    const char *name;

    if (!buf || size == 0)
        return -EINVAL;

    name = get_syscall_name(nr);
    if (!name)
        return -EINVAL;

    snprintf(buf, size, "__x64_sys_%s", name);

    return 0;
}

/* ================= GENERIC HOOK ================= */

static asmlinkage long generic_hook(const struct pt_regs *regs)
{
    int nr;
    struct syscall_hook *h;

    if (!regs)
        return -EINVAL;

    nr = regs->orig_ax;

    h = find_hook(nr);
    if (!h)
        return -EINVAL;

    printk(KERN_INFO "[HOOK] syscall %d (%s)\n", nr, h->name);

    /*
     * should_block() fa già il delay se serve.
     * Non bisogna fare un altro schedule_timeout qui.
     */
    should_block(nr);

    return ((asmlinkage long (*)(const struct pt_regs *))h->addr)(regs);
}

/* ================= FTRACE ================= */

static void notrace fh_ftrace_thunk(unsigned long ip,
                                    unsigned long parent_ip,
                                    struct ftrace_ops *ops,
                                    struct ftrace_regs *fregs)
{
    struct pt_regs *regs;

    regs = ftrace_get_regs(fregs);
    if (!regs)
        return;

    if (!within_module(parent_ip, THIS_MODULE))
        regs->ip = (unsigned long)generic_hook;
}

/* ================= API ================= */

int add_syscall_hook(int nr)
{
    struct syscall_hook *h;
    char symbol[64];
    unsigned long addr;
    int ret;

    if (hook_count >= MAX_HOOKS)
        return -ENOMEM;

    if (find_hook(nr))
        return -EEXIST;

    ret = syscall_symbol_from_nr(nr, symbol, sizeof(symbol));
    if (ret)
        return ret;

    addr = get_symbol_addr(symbol);
    if (!addr) {
        printk(KERN_ERR "[HOOK] symbol not found: %s\n", symbol);
        return -ENOENT;
    }

    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h)
        return -ENOMEM;

    h->nr = nr;
    strscpy(h->name, symbol, sizeof(h->name));
    h->addr = addr;
    h->active = false;

    h->ops.func = fh_ftrace_thunk;
    h->ops.flags = FTRACE_OPS_FL_SAVE_REGS |
                   FTRACE_OPS_FL_RECURSION |
                   FTRACE_OPS_FL_IPMODIFY;

    ret = ftrace_set_filter_ip(&h->ops, addr, 0, 0);
    if (ret) {
        printk(KERN_ERR "[HOOK] ftrace_set_filter_ip failed syscall=%d ret=%d\n",
               nr, ret);
        goto err_free;
    }

    ret = register_ftrace_function(&h->ops);
    if (ret) {
        printk(KERN_ERR "[HOOK] register_ftrace_function failed syscall=%d ret=%d\n",
               nr, ret);
        goto err_filter;
    }

    hooks[hook_count++] = h;
    h->active = true;

    printk(KERN_INFO "[HOOK] installed syscall=%d symbol=%s addr=%px\n",
           nr, h->name, (void *)addr);

    return 0;

err_filter:
    ftrace_set_filter_ip(&h->ops, addr, 1, 0);

err_free:
    kfree(h);
    return ret;
}

int remove_syscall_hook(int nr)
{
    int i;
    struct syscall_hook *h;

    for (i = 0; i < hook_count; i++) {
        if (hooks[i] && hooks[i]->nr == nr) {
            h = hooks[i];

            unregister_ftrace_function(&h->ops);
            ftrace_set_filter_ip(&h->ops, h->addr, 1, 0);

            printk(KERN_INFO "[HOOK] removed syscall=%d symbol=%s\n",
                   h->nr, h->name);

            kfree(h);

            hooks[i] = hooks[hook_count - 1];
            hooks[hook_count - 1] = NULL;
            hook_count--;

            return 0;
        }
    }

    return -ENOENT;
}

int setup_ftrace_hook(void)
{
    return 0;
}

void cleanup_ftrace_hook(void)
{
    int i;

    for (i = 0; i < hook_count; i++) {
        if (!hooks[i])
            continue;

        unregister_ftrace_function(&hooks[i]->ops);
        ftrace_set_filter_ip(&hooks[i]->ops, hooks[i]->addr, 1, 0);

        printk(KERN_INFO "[HOOK] cleanup syscall=%d symbol=%s\n",
               hooks[i]->nr,
               hooks[i]->name);

        kfree(hooks[i]);
        hooks[i] = NULL;
    }

    hook_count = 0;
}