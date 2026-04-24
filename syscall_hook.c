#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/syscalls.h>
#include <linux/cred.h>
#include <linux/sched.h>
#include <asm/paravirt.h>

#include "syscall_hook.h"
#include "monitor.h"
#include "registry.h"

#define MODNAME "syscall_monitor"

// ==============================
// GLOBALS
// ==============================

unsigned long **sys_call_table = NULL;
static unsigned long (*kallsyms_lookup_name_fn)(const char *name);

// syscall da hookare
static int hooked_syscalls[] = {
    __NR_getpid
};

#define HOOK_COUNT (sizeof(hooked_syscalls)/sizeof(int))

static void *original_syscalls[512];

// ==============================
// WRITE PROTECTION
// ==============================

static void disable_write_protection(void) {
    write_cr0(read_cr0() & (~0x10000));
}

static void enable_write_protection(void) {
    write_cr0(read_cr0() | 0x10000);
}

// ==============================
// RESOLVE SYMBOL
// ==============================

static int resolve_kallsyms_lookup_name(void) {
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };

    if (register_kprobe(&kp) < 0)
        return -1;

    kallsyms_lookup_name_fn = (void *)kp.addr;
    unregister_kprobe(&kp);

    return 0;
}

// ==============================
// SYSCALL TABLE
// ==============================

int resolve_syscall_table(void) {

    if (resolve_kallsyms_lookup_name() < 0)
        return -1;

    sys_call_table = (unsigned long **)
        kallsyms_lookup_name_fn("sys_call_table");

    if (!sys_call_table)
        return -1;

    printk(KERN_INFO "%s: syscall table found\n", MODNAME);
    return 0;
}

// ==============================
// CALL ORIGINAL
// ==============================

static inline long call_original(int nr, const struct pt_regs *regs) {
    return ((long (*)(const struct pt_regs *))
            original_syscalls[nr])(regs);
}

// ==============================
// WRAPPER
// ==============================



static asmlinkage long hooked_syscall(const struct pt_regs *regs) {

    int nr = regs->orig_ax;

    uid_t uid = current_uid().val;
    const char *comm = current->comm;

    if (!monitor_is_enabled())
        return call_original(nr, regs);

    if (!is_syscall_monitored(nr))
        return call_original(nr, regs);

    if (!is_uid_monitored(uid) && !is_comm_monitored(comm))
        return call_original(nr, regs);

    if (monitor_should_throttle()) {
        monitor_block_current();
    }

    return call_original(nr, regs);
}

// ==============================
// INSTALL
// ==============================

int install_syscall_hooks(void) {

    int i;

    if (!sys_call_table)
        return -1;

    disable_write_protection();

    for (i = 0; i < HOOK_COUNT; i++) {

        int nr = hooked_syscalls[i];

        original_syscalls[nr] = (void *)sys_call_table[nr];
        sys_call_table[nr] = (unsigned long *)hooked_syscall;
    }

    enable_write_protection();

    printk(KERN_INFO "%s: hooks installed\n", MODNAME);
    return 0;
}

// ==============================
// UNINSTALL
// ==============================

void uninstall_syscall_hooks(void) {

    int i;

    if (!sys_call_table)
        return;

    disable_write_protection();

    for (i = 0; i < HOOK_COUNT; i++) {

        int nr = hooked_syscalls[i];

        sys_call_table[nr] = (unsigned long *)original_syscalls[nr];
    }

    enable_write_protection();

    printk(KERN_INFO "%s: hooks removed\n", MODNAME);
}