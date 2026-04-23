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
// GLOBALI
// ==============================

unsigned long **sys_call_table = NULL;
static unsigned long (*kallsyms_lookup_name_fn)(const char *name);

// syscall che hookiamo
static int hooked_syscalls[] = {
    __NR_openat,
    __NR_read,
    __NR_write
};

#define HOOK_COUNT (sizeof(hooked_syscalls)/sizeof(int))

// originali
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
// RESOLVE kallsyms
// ==============================

static int resolve_kallsyms_lookup_name(void) {
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name"
    };

    if (register_kprobe(&kp) < 0)
        return -1;

    kallsyms_lookup_name_fn = (void *)kp.addr;
    unregister_kprobe(&kp);

    return 0;
}

// ==============================
// RESOLVE SYSCALL TABLE
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
// CALL ORIGINALE
// ==============================

static inline long call_original(int nr, const struct pt_regs *regs) {
    return ((long (*)(const struct pt_regs *))
            original_syscalls[nr])(regs);
}

// ==============================
// WRAPPER GENERICO
// ==============================

asmlinkage long hooked_syscall(const struct pt_regs *regs) {

    int nr = regs->orig_ax;

    uid_t uid = current_uid().val;
    const char *comm = current->comm;

    // DEBUG
    printk(KERN_INFO "%s: syscall %d called by %s\n", MODNAME, nr, comm);

    // filtro syscall
    if (!is_syscall_monitored(nr))
        return call_original(nr, regs);

    // filtro entità
    if (!is_uid_monitored(uid) && !is_comm_monitored(comm))
        return call_original(nr, regs);

    // throttling
    if (monitor_should_throttle(uid, comm)) {
        printk(KERN_INFO "%s: throttling syscall %d\n", MODNAME, nr);
        monitor_block_current();
    }

    return call_original(nr, regs);
}

// ==============================
// INSTALL HOOK
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

        printk(KERN_INFO "%s: hooked syscall %d\n", MODNAME, nr);
    }

    enable_write_protection();

    return 0;
}

// ==============================
// UNINSTALL HOOK
// ==============================

void uninstall_syscall_hooks(void) {

    int i;

    if (!sys_call_table)
        return;

    disable_write_protection();

    for (i = 0; i < HOOK_COUNT; i++) {

        int nr = hooked_syscalls[i];

        sys_call_table[nr] = (unsigned long *)original_syscalls[nr];

        printk(KERN_INFO "%s: restored syscall %d\n", MODNAME, nr);
    }

    enable_write_protection();
}