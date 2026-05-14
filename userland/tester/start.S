/*
 * tester — entry point.
 *
 * Spawned by taskman (see userland/taskman/spawn.c).
 *
 * Register state on entry (v0.4.4):
 *   a0 = pid       (set by taskman's TCB_WriteRegisters)
 *   sp = top of the SysV ABI initial-stack image:
 *          0(sp)        argc
 *          8(sp)        argv[0]
 *          ...          argv[argc-1]
 *          .            argv NULL terminator
 *          .            envp[0]
 *          ...          envp[envc-1]
 *          .            envp NULL terminator
 *          .            auxv (AT_NULL terminator)
 *          .            argv/envp string area
 *
 * crt0 just initialises gp/tp/fp, computes argc/argv/envp into a1/a2/a3,
 * and calls the shared _qsoe_start_main C shim (libqsoe/src/start_main.c).
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

    /* Plant tp = &qsoe_main_tcb (see taskman/start.S for rationale). */
    la tp, qsoe_main_tcb

    /* Clear frame pointer (no caller). */
    li fp, 0

    /* Re-arrange registers for _qsoe_start_main(pid, argc, argv, envp).
     * a0 already holds pid from the kernel. Compute argc/argv/envp from
     * sp and stage them into a1/a2/a3. */
    ld   a1, 0(sp)          /* argc */
    addi a2, sp, 8          /* argv = &sp[1] */
    addi t0, a1, 1          /* argc + 1 (for argv's NULL terminator) */
    slli t0, t0, 3          /* (argc+1) * 8 */
    add  a3, a2, t0         /* envp = argv + (argc+1)*8 */

    call _qsoe_start_main

    /* _qsoe_start_main calls _exit and does not return; this is just
     * a guard against a future code path that might. */
1:  j 1b

    .size _start, . - _start
