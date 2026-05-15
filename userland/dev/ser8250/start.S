/*
 * devc-ser8250 — entry point.
 * Same crt0 shape as hello/init/tester.
 */

.section .text.start
.global _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    la tp, qsoe_main_tcb

    li fp, 0

    /* a0 = pid (preserved). Stage argc/argv/envp into a1/a2/a3. */
    ld   a1, 0(sp)
    addi a2, sp, 8
    addi t0, a1, 1
    slli t0, t0, 3
    add  a3, a2, t0

    call _qsoe_start_main

1:  j 1b

    .size _start, . - _start
