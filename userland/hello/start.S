/*
 * hello — entry point.
 *
 * Spawned by another QSOE process via posix_spawn(). a0 holds the
 * child's pid on entry (set by taskman's WriteRegisters). On return
 * from main(), crt0 falls through to _exit(return_value) so the
 * proper ProcessTerminate wire path runs.
 */

.section .text.start
.global _start
_start:
    /* Initialise gp (RISC-V small data/bss). */
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* Plant tp = &qsoe_main_tcb (libqsoe per-thread state). */
    la tp, qsoe_main_tcb

    /* Set up stack. */
    la sp, _stack_top

    /* Clear frame pointer. */
    li fp, 0

    /* a0 already holds our pid; pass through to main(pid_t). */
    call main

    /* main returned — funnel its return value into _exit(). */
    call _exit
1:  j 1b

    .size _start, . - _start

.section .bss
    .align 12
_stack_bottom:
    .skip 8192
_stack_top:
