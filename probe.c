#include <linux/kprobes.h>
#include <linux/kernel.h>

#include "probe.h"

typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);

static kallsyms_lookup_name_t kallsyms_lookup_name_ptr;

int resolve_kallsyms_lookup_name(void)
{
    int ret;

    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name",
    };

    ret = register_kprobe(&kp);
    if (ret < 0) {
        printk(KERN_ERR "[PROBE] register_kprobe failed ret=%d\n", ret);
        return ret;
    }

    kallsyms_lookup_name_ptr = (kallsyms_lookup_name_t)kp.addr;

    unregister_kprobe(&kp);

    if (!kallsyms_lookup_name_ptr) {
        printk(KERN_ERR "[PROBE] kallsyms_lookup_name not found\n");
        return -ENOENT;
    }

    printk(KERN_INFO "[PROBE] kallsyms_lookup_name resolved at %px\n",
           (void *)kallsyms_lookup_name_ptr);

    return 0;
}

unsigned long get_symbol_addr(const char *name)
{
    if (!kallsyms_lookup_name_ptr)
        return 0;

    return kallsyms_lookup_name_ptr(name);
}