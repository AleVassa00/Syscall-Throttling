#define EXPORT_SYMTAB

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/preempt.h>
#include <linux/rwlock.h>
#include <linux/kprobes.h>
#include <linux/sched.h>
#include <linux/errno.h>

#include <asm/processor-flags.h>
#include <asm/special_insns.h>
#include <asm/unistd.h>

#include "syscall_hook.h"
#include "probe.h"
#include "monitor.h"

#define MODNAME        "SYSCALL_MONITOR"
#define MAX_HOOKS      __NR_syscalls
#define MAX_SYSCALL_NR __NR_syscalls
#define INST_LEN       5

struct syscall_hook {
    int nr;
    unsigned long original;
    bool active;
};

/*
 * hooks[nr] contiene la struttura associata alla syscall nr.
 * Questo evita la ricerca lineare e rende il lookup O(1).
 */
static struct syscall_hook *hooks[MAX_HOOKS];
static int hook_count;
static DEFINE_RWLOCK(hooks_lock);

static unsigned long **sys_call_table;

/* Patch x64_sys_call */
static char jump_inst[INST_LEN];
static char original_inst[INST_LEN];
static unsigned long x64_sys_call_addr;
static bool x64_sys_call_patched;

/* CR0 / CR4 */
static unsigned long cr0_saved;
static unsigned long cr4_saved;

/* ================= Memory protection helpers ================= */

static inline void write_cr0_forced(unsigned long val)
{
    unsigned long __force_order;

    asm volatile("mov %0, %%cr0"
                 : "+r"(val), "+m"(__force_order));
}

static inline void write_cr4_forced(unsigned long val)
{
    unsigned long __force_order;

    asm volatile("mov %0, %%cr4"
                 : "+r"(val), "+m"(__force_order));
}

static inline void unprotect_memory(void)
{
    write_cr0_forced(cr0_saved & ~X86_CR0_WP);
}

static inline void protect_memory(void)
{
    write_cr0_forced(cr0_saved);
}

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

/*Disabilita temporaneamente le protezioni hardware necessarie per modificare codice kernel read-only
 *- disabilita la preemption
 *- salva CR0/CR4
 *- disabilita CET se presente
 *- rimuove il bit Write Protect da CR0
*/
static inline void begin_syscall_table_hack(void)
{
    preempt_disable();

    cr0_saved = read_cr0();
    cr4_saved = native_read_cr4();

    conditional_cet_disable();
    unprotect_memory();
}
/*Ripristina le protezioni hardware precedentemente disabilitate da begin_syscall_table_hack()

 */static inline void end_syscall_table_hack(void)
{
    protect_memory();
    conditional_cet_enable();

    preempt_enable();
}

/*Dispatcher custom utilizzato dopo la patch di x64_sys_call
 La funzione recupera dalla syscall table la funzione associata al numero nr ed effettua
 un jump diretto verso la syscall selezionata
 Non ritorna mai al chiamante.
*/
static __always_inline void __noreturn call(struct pt_regs *regs,unsigned int nr)
{
    asm volatile(
        "mov (%1, %0, 8), %%rax\n\t"
        "jmp __x86_indirect_thunk_rax\n\t"
        :
        : "r"((long)nr), "r"(sys_call_table)
        : "rax"
    );

    __builtin_unreachable();
}

/* ================= Hook registry ================= */
/*Restituisce il puntatore alla syscall originale associata al numero nr.
 -Il lookup avviene in O(1) tramite l'array hooks[nr].
 -Il read_lock protegge l'accesso concorrente rispetto ad add/remove hook e cleanup del modulo.
*/
static unsigned long get_original_syscall(int nr)
{
    struct syscall_hook *h;
    unsigned long original = 0;

    if (nr < 0 || nr >= MAX_HOOKS)
        return 0;

    read_lock(&hooks_lock);

    h = hooks[nr];
    if (h && h->active){
        original = h->original;
    }

    read_unlock(&hooks_lock);

    return original;
}

/*Wrapper generico installato nella syscall table per tutte le syscall monitorate.
  - recupera il numero della syscall corrente
  - ottiene il puntatore originale salvato
  - applica il monitor/throttling
  - esegue la syscall reale
 Se il throttling viene interrotto da un segnale, la syscall originale NON viene invocata e viene restituito l'errore propagato dal monitor.
*/
static asmlinkage long generic_syscall_hook(const struct pt_regs *regs)
{
    int nr;
    int throttle_ret;
    unsigned long original;
    long ret;

    if (!regs)
        return -EINVAL;

    if (!try_module_get(THIS_MODULE))
        return -ENODEV;

    nr = regs->orig_ax;

    original = get_original_syscall(nr);
    if (!original) {
        ret = -ENOSYS;
        goto out;
    }

    throttle_ret = should_block(nr);
    if (throttle_ret) {
        ret = throttle_ret;
        goto out;
    }

    ret = ((asmlinkage long (*)(const struct pt_regs *))original)(regs);

out:
    module_put(THIS_MODULE);
    return ret;
}


/*Installa la patch su x64_sys_call
 - risolve l'indirizzo di x64_sys_call
 - salva le istruzioni originali
 - costruisce un jump relativo verso call()
 - rende temporaneamente scrivibile il codice kernel
 - applica la patch
 */
static int install_syscall_dispatcher_patch(void)
{
    long raw_offset;
    int offset;

    if (x64_sys_call_patched)
        return 0;

    x64_sys_call_addr = get_symbol_addr("x64_sys_call");
    if (!x64_sys_call_addr) {
        printk(KERN_ERR "%s: cannot resolve x64_sys_call\n",
               MODNAME);
        return -ENOENT;
    }

    memcpy(original_inst, (void *)x64_sys_call_addr, INST_LEN);

    jump_inst[0] = 0xE9;

    raw_offset = (long)((unsigned long)call -
                        x64_sys_call_addr -
                        INST_LEN);

    if (raw_offset > INT_MAX || raw_offset < INT_MIN) {
        printk(KERN_ERR "%s: jump offset out of range: %ld\n",
               MODNAME, raw_offset);
        return -ERANGE;
    }

    offset = (int)raw_offset;
    memcpy(jump_inst + 1, &offset, sizeof(int));

    printk(KERN_INFO "%s: x64_sys_call=%px call=%px offset=%d\n",
           MODNAME,
           (void *)x64_sys_call_addr,
           (void *)call,
           offset);

    begin_syscall_table_hack();
    memcpy((void *)x64_sys_call_addr, jump_inst, INST_LEN);
    end_syscall_table_hack();

    x64_sys_call_patched = true;

    printk(KERN_INFO "%s: x64_sys_call patched\n", MODNAME);

    return 0;
}
/*Ripristina le istruzioni originali di x64_sys_call,rimuovendo la patch installata dal modulo
  La funzione ripristina il codice originale e riabilita il normale dispatcher del kernel
 */
static void remove_syscall_dispatcher_patch(void)
{
    if (!x64_sys_call_patched || !x64_sys_call_addr)
        return;

    begin_syscall_table_hack();
    memcpy((void *)x64_sys_call_addr, original_inst, INST_LEN);
    end_syscall_table_hack();

    x64_sys_call_patched = false;
    x64_sys_call_addr = 0;

    printk(KERN_INFO "%s: x64_sys_call restored\n", MODNAME);
}


/*Inizializza il sistema di hooking delle syscall
  - inizializza le strutture interne
  - risolve sys_call_table
  - installa la patch del dispatcher
 */
int syscall_hook_init(void)
{
    int ret;

    memset(hooks, 0, sizeof(hooks));

    hook_count = 0;
    sys_call_table = NULL;
    x64_sys_call_addr = 0;
    x64_sys_call_patched = false;

    sys_call_table = (unsigned long **)get_symbol_addr("sys_call_table");
    if (!sys_call_table) {
        printk(KERN_ERR "%s: sys_call_table not found\n", MODNAME);
        return -ENOENT;
    }

    printk(KERN_INFO "%s: sys_call_table at %px\n",
           MODNAME, sys_call_table);

    ret = install_syscall_dispatcher_patch();
    if (ret) {
        printk(KERN_ERR "%s: install_syscall_dispatcher_patch failed ret=%d\n",
               MODNAME, ret);
        sys_call_table = NULL;
        return ret;
    }

    return 0;
}

/*Cleanup completo del sistema di hooking.
 - ripristina tutte le syscall originali
 - libera le strutture allocate
 - rimuove la patch da x64_sys_call
*/

void syscall_hook_cleanup(void)
{
    int i;

    if (!sys_call_table)
        return;

    write_lock(&hooks_lock);

    begin_syscall_table_hack();

    for (i = 0; i < MAX_HOOKS; i++) {
        if (!hooks[i])
            continue;

        if (hooks[i]->active) {
            sys_call_table[hooks[i]->nr] =
                (unsigned long *)hooks[i]->original;

            hooks[i]->active = false;
        }
    }

    end_syscall_table_hack();

    for (i = 0; i < MAX_HOOKS; i++) {
        kfree(hooks[i]);
        hooks[i] = NULL;
    }

    hook_count = 0;

    write_unlock(&hooks_lock);

    remove_syscall_dispatcher_patch();

    sys_call_table = NULL;

    printk(KERN_INFO "%s: syscall hooks cleaned up\n", MODNAME);
}

/*Installa l'hook sulla syscall nr
  - salva il puntatore originale
  - sostituisce la syscall nella syscall table con generic_syscall_hook
  - registra l'hook nella struttura interna
 L'accesso concorrente alla tabella hooks[] è protetto tramite write_lock.
*/

int add_syscall_hook(int nr)
{
    struct syscall_hook *h;

    if (!sys_call_table || nr < 0 || nr >= MAX_SYSCALL_NR)
        return -EINVAL;

    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h)
        return -ENOMEM;

    write_lock(&hooks_lock);

    if (hooks[nr]) {
        write_unlock(&hooks_lock);
        kfree(h);
        return -EEXIST;
    }

    if (hook_count >= MAX_HOOKS) {
        write_unlock(&hooks_lock);
        kfree(h);
        return -ENOMEM;
    }

    h->nr = nr;
    h->original = (unsigned long)sys_call_table[nr];
    h->active = false;

    printk(KERN_INFO "%s: before hook table[%d]=%px\n",
           MODNAME, nr, sys_call_table[nr]);

    begin_syscall_table_hack();
    sys_call_table[nr] = (unsigned long *)generic_syscall_hook;
    end_syscall_table_hack();

    h->active = true;
    hooks[nr] = h;
    hook_count++;

    printk(KERN_INFO "%s: hooked nr=%d original=%px new=%px\n",
           MODNAME, nr, (void *)h->original, (void *)generic_syscall_hook);

    write_unlock(&hooks_lock);

    return 0;
}
/*Rimuove l'hook dalla syscall nr.
 - ripristina la syscall originale
 - rimuove l'entry dalla struttura hooks[]
 - libera la memoria associata all'hook
*/
int remove_syscall_hook(int nr)
{
    struct syscall_hook *h;

    if (!sys_call_table || nr < 0 || nr >= MAX_SYSCALL_NR)
        return -EINVAL;

    write_lock(&hooks_lock);

    h = hooks[nr];
    if (!h) {
        write_unlock(&hooks_lock);
        return -ENOENT;
    }

    begin_syscall_table_hack();

    sys_call_table[nr] = (unsigned long *)h->original;

    end_syscall_table_hack();

    h->active = false;
    hooks[nr] = NULL;
    hook_count--;

    printk(KERN_INFO "%s: unhooked nr=%d restored=%px\n",
           MODNAME, nr, (void *)h->original);

    kfree(h);

    write_unlock(&hooks_lock);

    return 0;
}