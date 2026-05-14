/*
 * hello — entry point.
 *
 * Spawned by another QSOE process via posix_spawn(). The kernel-supplied
 * sp points at the SysV ABI initial-stack image taskman built; see
 * tester/start.S for the layout commentary. crt0 just sets up
 * gp/tp/fp, computes argc/argv/envp into a1/a2/a3, and tail-calls
 * _qsoe_start_main.
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
