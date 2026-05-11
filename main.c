#include <linux/module.h>
#include <linux/kernel.h>

#include "probe.h"
#include "syscall_hook.h"
#include "device.h"
#include "monitor.h"
#include "registry.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Manuel");
MODULE_DESCRIPTION("Syscall throttling monitor");

static int __init mod_init(void)
{
    int ret;

    printk(KERN_INFO "[MOD] loading syscall monitor\n");

    ret = resolve_kallsyms_lookup_name(); //ok
    if (ret < 0) {
        printk(KERN_ERR "[MOD] failed to resolve kallsyms_lookup_name ret=%d\n", ret);
        return ret;
    }

    ret = monitor_init(); //ok
    if (ret < 0) {
        printk(KERN_ERR "[MOD] monitor_init failed ret=%d\n", ret);
        return ret;
    }

    ret = syscall_hook_init(); // ok
    if (ret < 0) {
        printk(KERN_ERR "[MOD] syscall_hook_init failed ret=%d\n", ret);
        monitor_cleanup();
        return ret;
    }

    ret = device_init(); //ok
    if (ret < 0) {
        printk(KERN_ERR "[MOD] device_init failed ret=%d\n", ret);
        syscall_hook_cleanup();
        monitor_cleanup();
        return ret;
    }

    printk(KERN_INFO "[MOD] module loaded successfully\n");

    return 0;
}

static void __exit mod_exit(void)
{
    printk(KERN_INFO "[MOD] unloading syscall monitor\n");

    device_cleanup();

    syscall_hook_cleanup();

    monitor_cleanup();

    registry_cleanup();

    printk(KERN_INFO "[MOD] module unloaded\n");
}

module_init(mod_init);
module_exit(mod_exit);