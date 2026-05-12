/*
 * taskman — entry point.
 *
 * The seL4 kernel jumps here in U-mode after rootserver setup.
 * Registers on entry (per seL4 ABI):
 *   a0 = bootinfo pointer
 * For now we ignore bootinfo and just hand off to C main.
 */

.section .text.start
.global _start
_start:
    /* Set up stack pointer. */
    la sp, _stack_top

    /* Clear frame pointer (no caller). */
    li fp, 0

    call main

    /* main returned — spin (wfi traps in U-mode on RISC-V). */
1:  j 1b

    .size _start, . - _start

.section .bss
    .align 12
_stack_bottom:
    .skip 8192
_stack_top:
