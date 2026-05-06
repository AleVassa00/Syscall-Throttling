#define EXPORT_SYMTAB

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/preempt.h>
#include <linux/rwlock.h>
#include <linux/kprobes.h>

#include <asm/processor-flags.h>
#include <asm/special_insns.h>

#include "syscall_hook.h"
#include "probe.h"
#include "monitor.h"

#define MODNAME        "SYSCALL_MONITOR"
#define MAX_HOOKS      512
#define MAX_SYSCALL_NR 512
#define INST_LEN       5

/* trampolino assembly - definito in trampoline.S */
asmlinkage void asm_syscall_call(struct pt_regs *regs,
                                  unsigned int nr,
                                  unsigned long **table);

struct syscall_hook {
    int nr;
    unsigned long original;
    int active;
};

static struct syscall_hook *hooks[MAX_HOOKS];
static int hook_count;
static DEFINE_RWLOCK(hooks_lock);
static unsigned long **sys_call_table;

/* patch x64_sys_call */
static char jump_inst[INST_LEN];
static char original_inst[INST_LEN];
static unsigned long x64_sys_call_addr;
static int x64_sys_call_patched;

/* ================= CR0 / CR4 helpers ================= */

static unsigned long cr0_saved;
static unsigned long cr4_saved;

static inline void write_cr0_forced(unsigned long val)
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr0" : "+r"(val), "+m"(__force_order));
}

static inline void write_cr4_forced(unsigned long val)
{
    unsigned long __force_order;
    asm volatile("mov %0, %%cr4" : "+r"(val), "+m"(__force_order));
}

static inline void unprotect_memory(void) { write_cr0_forced(cr0_saved & ~X86_CR0_WP); }
static inline void protect_memory(void)   { write_cr0_forced(cr0_saved); }

static inline void conditional_cet_disable(void)
{
#ifdef X86_CR4_CET
    if (cr4_saved & X86_CR4_CET)
        write_cr4_forced(cr4_saved & ~X86_CR4_CET);
#endif
}

static inline void conditional_cet_enable(void)
{
#ifdef X86_CR4_CET
    if (cr4_saved & X86_CR4_CET)
        write_cr4_forced(cr4_saved);
#endif
}

static inline void begin_syscall_table_hack(void)
{
    preempt_disable();
    cr0_saved = read_cr0();
    cr4_saved = native_read_cr4();
    conditional_cet_disable();
    unprotect_memory();
}

static inline void end_syscall_table_hack(void)
{
    protect_memory();
    conditional_cet_enable();
    preempt_enable();
}

/* ================= Hook registry ================= */

static struct syscall_hook *find_hook_nolock(int nr)
{
    int i;
    for (i = 0; i < hook_count; i++)
        if (hooks[i] && hooks[i]->nr == nr)
            return hooks[i];
    return NULL;
}

static unsigned long get_original_syscall(int nr)
{
    struct syscall_hook *h;
    unsigned long original = 0;

    read_lock(&hooks_lock);
    h = find_hook_nolock(nr);
    if (h && h->active)
        original = h->original;
    read_unlock(&hooks_lock);

    return original;
}

/* ================= Generic syscall wrapper ================= */

static asmlinkage long generic_syscall_hook(const struct pt_regs *regs)
{
    int nr;
    unsigned long original;

    if (!regs)
        return -EINVAL;

    nr = regs->orig_ax;

    printk(KERN_INFO "[HOOK] called nr=%d comm=%s\n", nr, current->comm);

    original = get_original_syscall(nr);
    if (!original) {
        printk(KERN_ERR "%s: original syscall not found for nr=%d\n",
               MODNAME, nr);
        return -ENOSYS;
    }

    should_block(nr);

    return ((asmlinkage long (*)(const struct pt_regs *))original)(regs);
}

/* ================= Patch x64_sys_call ================= */

static int patch_x64_sys_call(void)
{
    int offset;
    struct kprobe kp = { .symbol_name = "x64_sys_call" };

    /* uso kprobe solo per risolvere l'indirizzo, poi la deregistro */
    if (register_kprobe(&kp)) {
        printk(KERN_ERR "%s: cannot resolve x64_sys_call\n", MODNAME);
        return -ENOENT;
    }
    x64_sys_call_addr = (unsigned long)kp.addr;
    unregister_kprobe(&kp);

    memcpy(original_inst, (void *)x64_sys_call_addr, INST_LEN);

    jump_inst[0] = 0xE9;
    offset = (unsigned long)asm_syscall_call - x64_sys_call_addr - INST_LEN;
    memcpy(jump_inst + 1, &offset, sizeof(int));

    begin_syscall_table_hack();
    memcpy((void *)x64_sys_call_addr, jump_inst, INST_LEN);
    end_syscall_table_hack();

    x64_sys_call_patched = 1;

    printk(KERN_INFO "%s: x64_sys_call patched at %px\n",
           MODNAME, (void *)x64_sys_call_addr);
    return 0;
}

static void restore_x64_sys_call(void)
{
    if (!x64_sys_call_patched || !x64_sys_call_addr)
        return;

    begin_syscall_table_hack();
    memcpy((void *)x64_sys_call_addr, original_inst, INST_LEN);
    end_syscall_table_hack();

    x64_sys_call_patched = 0;
    printk(KERN_INFO "%s: x64_sys_call restored\n", MODNAME);
}

/* ================= Public API ================= */

int syscall_hook_init(void)
{
    int ret;

    hook_count = 0;
    sys_call_table = NULL;
    x64_sys_call_patched = 0;
    x64_sys_call_addr = 0;

    sys_call_table = (unsigned long **)get_symbol_addr("sys_call_table");
    if (!sys_call_table) {
        printk(KERN_ERR "%s: sys_call_table not found\n", MODNAME);
        return -ENOENT;
    }

    printk(KERN_INFO "%s: sys_call_table at %px\n", MODNAME, sys_call_table);

    ret = patch_x64_sys_call();
    if (ret) {
        printk(KERN_ERR "%s: patch failed ret=%d\n", MODNAME, ret);
        return ret;
    }

    return 0;
}

void syscall_hook_cleanup(void)
{
    int i;

    if (!sys_call_table)
        return;

    write_lock(&hooks_lock);
    begin_syscall_table_hack();

    for (i = 0; i < hook_count; i++) {
        if (!hooks[i]) continue;
        if (hooks[i]->active) {
            sys_call_table[hooks[i]->nr] = (unsigned long *)hooks[i]->original;
            hooks[i]->active = 0;
        }
    }

    end_syscall_table_hack();

    for (i = 0; i < hook_count; i++) {
        kfree(hooks[i]);
        hooks[i] = NULL;
    }
    hook_count = 0;

    write_unlock(&hooks_lock);

    restore_x64_sys_call();
    sys_call_table = NULL;
}

int add_syscall_hook(int nr)
{
    struct syscall_hook *h;

    if (!sys_call_table || nr < 0 || nr >= MAX_SYSCALL_NR)
        return -EINVAL;

    write_lock(&hooks_lock);

    if (hook_count >= MAX_HOOKS) { write_unlock(&hooks_lock); return -ENOMEM; }
    if (find_hook_nolock(nr))    { write_unlock(&hooks_lock); return -EEXIST; }

    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h) { write_unlock(&hooks_lock); return -ENOMEM; }

    h->nr       = nr;
    h->original = (unsigned long)sys_call_table[nr];
    h->active   = 0;

    begin_syscall_table_hack();
    sys_call_table[nr] = (unsigned long *)generic_syscall_hook;
    end_syscall_table_hack();

    h->active = 1;
    hooks[hook_count++] = h;

    write_unlock(&hooks_lock);

    printk(KERN_INFO "%s: hooked nr=%d original=%px\n",
           MODNAME, nr, (void *)h->original);
    return 0;
}

int remove_syscall_hook(int nr)
{
    int i;
    struct syscall_hook *h;

    if (!sys_call_table)
        return -EINVAL;

    write_lock(&hooks_lock);

    for (i = 0; i < hook_count; i++) {
        if (!hooks[i] || hooks[i]->nr != nr) continue;

        h = hooks[i];

        begin_syscall_table_hack();
        sys_call_table[nr] = (unsigned long *)h->original;
        end_syscall_table_hack();

        kfree(h);
        hooks[i] = hooks[hook_count - 1];
        hooks[hook_count - 1] = NULL;
        hook_count--;

        write_unlock(&hooks_lock);
        return 0;
    }

    write_unlock(&hooks_lock);
    return -ENOENT;
}