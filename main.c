#include <linux/module.h>
#include <linux/kernel.h>

#include "syscall_hook.h"
#include "device.h"
#include "registry.h"
#include "monitor.h"
#include "stats.h"

#define MODNAME "syscall_monitor"

static int __init syscall_monitor_init(void) {
    int ret;

    printk(KERN_INFO "%s: init\n", MODNAME);

    // 1️⃣ Resolve sys_call_table
    ret = resolve_syscall_table();
    if (ret < 0) {
        printk(KERN_ERR "%s: failed to resolve syscall table\n", MODNAME);
        return ret;
    }

    // 2️⃣ Init registry (UID, programmi, syscall)
    ret = registry_init();
    if (ret < 0)
        goto err_registry;

    // 3️⃣ Init stats
    ret = stats_init();
    if (ret < 0)
        goto err_stats;

    // 4️⃣ Init monitor (throttling logic)
    ret = monitor_init();
    if (ret < 0)
        goto err_monitor;

    // 5️⃣ Hook syscall
    ret = install_syscall_hooks();
    if (ret < 0)
        goto err_hooks;

    // 6️⃣ Create device (/dev/syscall_monitor)
    ret = device_init();
    if (ret < 0)
        goto err_device;

    // 7️⃣ Monitor OFF di default
    monitor_set_enabled(0);

    printk(KERN_INFO "%s: loaded successfully\n", MODNAME);
    return 0;

err_device:
    uninstall_syscall_hooks();
err_hooks:
    monitor_cleanup();
err_monitor:
    stats_cleanup();
err_stats:
    registry_cleanup();
err_registry:
    return ret;
}


static void __exit syscall_monitor_exit(void) {

    printk(KERN_INFO "%s: exit\n", MODNAME);

    // ordine inverso rispetto init
    device_cleanup();
    uninstall_syscall_hooks();
    monitor_cleanup();
    stats_cleanup();
    registry_cleanup();

    printk(KERN_INFO "%s: unloaded\n", MODNAME);
}

module_init(syscall_monitor_init);
module_exit(syscall_monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("AleVassa00");
MODULE_DESCRIPTION("Syscall Throttling LKM");