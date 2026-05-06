#include <linux/linkage.h>

/*
 * asm_syscall_call viene chiamata con JMP da x64_sys_call.
 * A questo punto i registri sono:
 *   rdi = struct pt_regs *regs
 *   rsi = unsigned int nr
 *   r10 = unsigned long **sys_call_table  (caricato da syscall_hook prima del patch)
 *
 * Fa: rax = sys_call_table[nr]; jmp rax (via retpoline)
 */
SYM_FUNC_START(asm_syscall_call)
    mov    (%r10, %rsi, 8), %rax
    jmp    __x86_indirect_thunk_rax
SYM_FUNC_END(asm_syscall_call)