/*
 * tester — entry point.
 *
 * Spawned by taskman (see userland/taskman/spawn.c). Per QSOE's
 * convention, taskman writes the new process's pid into a0 before
 * resuming the TCB, so a0 holds our pid on entry. We pass it through
 * to main().
 */

.section .text.start
.global _start
_start:
    /* Initialise the RISC-V global pointer (gp). The compiler uses
     * gp-relative addressing for small data/bss. */
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* Set up stack pointer. */
    la sp, _stack_top

    /* Clear frame pointer (no caller). */
    li fp, 0

    /* a0 already holds our pid (passed by taskman's WriteRegisters);
     * leave it as the first arg to main(pid_t). */
    call main

    /* main returned — spin. */
1:  j 1b

    .size _start, . - _start

.section .bss
    .align 12
_stack_bottom:
    .skip 8192
_stack_top:
