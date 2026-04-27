#include <linux/kprobes.h>
#include <linux/kernel.h>
#include "kprobe_helper.h"

static unsigned long (*kallsyms_lookup_name_fn)(const char *name);

unsigned long resolve_kallsyms_lookup_name(void)
{
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name"
    };

    int ret = register_kprobe(&kp);
    if (ret < 0) {
        printk(KERN_ERR "kprobe_helper: register_kprobe failed %d\n", ret);
        return ret;
    }

    kallsyms_lookup_name_fn = (void *)kp.addr;

    unregister_kprobe(&kp);

    if (!kallsyms_lookup_name_fn) {
        printk(KERN_ERR "kprobe_helper: kallsyms not found\n");
        return -1;
    }

    printk(KERN_INFO "kprobe_helper: kallsyms resolved\n");
    return 0;
}

unsigned long (*get_kallsyms_lookup_name_fn(void))(const char *name)
{
    return kallsyms_lookup_name_fn;
}