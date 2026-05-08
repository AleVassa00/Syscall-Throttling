#include <linux/kprobes.h>
#include <linux/kernel.h>
#include <linux/errno.h>

#include "probe.h"

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);

static kallsyms_lookup_name_t kallsyms_lookup_name_ptr;

static unsigned long resolve_symbol_kprobe(const char *name)
{
    struct kprobe kp = {
        .symbol_name = name,
    };

    int ret;
    unsigned long addr;

    if (!name)
        return 0;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        printk(KERN_ERR "[PROBE] register_kprobe failed symbol=%s ret=%d\n",
               name, ret);
        return 0;
    }

    addr = (unsigned long)kp.addr;

    unregister_kprobe(&kp);

    if (!addr) {
        printk(KERN_ERR "[PROBE] symbol not found: %s\n", name);
        return 0;
    }

    printk(KERN_INFO "[PROBE] resolved %s at %px\n",
           name, (void *)addr);

    return addr;
}

int resolve_kallsyms_lookup_name(void)
{
    unsigned long addr;

    addr = resolve_symbol_kprobe("kallsyms_lookup_name");
    if (!addr)
        return -ENOENT;

    kallsyms_lookup_name_ptr = (kallsyms_lookup_name_t)addr;

    return 0;
}


unsigned long get_symbol_addr(const char *name)
{
    if (!kallsyms_lookup_name_ptr)
        return 0;

    return kallsyms_lookup_name_ptr(name);
}