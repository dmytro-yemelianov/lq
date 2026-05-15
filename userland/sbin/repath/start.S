/*
 * userland/sbin/repath — entry point.
 *
 * Standard QSOE crt0: set gp / tp / fp, stage argc / argv / envp
 * into a1 / a2 / a3, then jump to libqsoe's _qsoe_start_main.
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

    ld   a1, 0(sp)
    addi a2, sp, 8
    addi t0, a1, 1
    slli t0, t0, 3
    add  a3, a2, t0

    call _qsoe_start_main

1:  j 1b

    .size _start, . - _start
