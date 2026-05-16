/*
 * crt0.S — QSOE userland entry point (the canonical version).
 *
 * Linked into every spawned userland program (qsh, devc-ser8250,
 * sbin/pipe, sbin/repath, tester, ...).  Built once by libqsoe's
 * Makefile into $(BUILD)/crt0.o.
 *
 * taskman is its own thing: see userland/taskman/start.S.  taskman
 * is the rootserver and enters with a different contract (a0 =
 * seL4_BootInfo *, no stack image laid out, libqsoe init deferred
 * until MM is up).  Keeping it as a separate file is honest.
 *
 * Register state on entry (set up by taskman in proc/spawn.c via
 * TCB_WriteRegisters + the SysV initial-stack image it builds):
 *
 *   a0 = pid
 *   sp = top of the RISC-V SysV ABI initial-stack image:
 *          0(sp)         argc
 *          8(sp)         argv[0]
 *          ...           argv[argc-1]
 *          .             argv NULL terminator
 *          .             envp[0]
 *          ...           envp[envc-1]
 *          .             envp NULL terminator
 *          .             auxv (AT_NULL terminator)
 *          .             argv/envp string area
 *
 * crt0 sets gp/tp/fp, computes argc/argv/envp into a1/a2/a3, then
 * jumps to _qsoe_start_main (libqsoe/src/start_main.c).  The shim
 * initialises libqsoe (TCB, slot table, qerrno) BEFORE handing off
 * to main — otherwise the first errno or IPC call after main()
 * blows up.
 *
 * .option norelax around `la gp, __global_pointer$` is required:
 * the assembler would otherwise relax that load into a gp-relative
 * addressing mode, which is circular (gp doesn't exist yet).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

.section .text.start
.global _start
_start:
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* tp -> qsoe_main_tcb so libqsoe's per-thread accessors (in
     * <qsoe/tls.h>) resolve.  qsoe_main_tcb lives in BSS, so any
     * .ipcbuf / .qerrno read before qsoe_libqsoe_init() sees zeros
     * — the correct uninitialised state. */
    la tp, qsoe_main_tcb

    /* No caller. */
    li fp, 0

    /* a0 = pid (kernel-set, preserved).  Stage argc/argv/envp into
     * a1/a2/a3 from the SysV stack image. */
    ld   a1, 0(sp)          /* argc                                */
    addi a2, sp, 8          /* argv = &sp[1]                       */
    addi t0, a1, 1          /* argc + 1 (for argv's NULL terminator) */
    slli t0, t0, 3          /* (argc+1) * 8                        */
    add  a3, a2, t0         /* envp = argv + (argc+1)*8            */

    call _qsoe_start_main

    /* _qsoe_start_main is _Noreturn (calls _exit).  This is just a
     * guard against a future code path that might return. */
1:  j 1b

    .size _start, . - _start
