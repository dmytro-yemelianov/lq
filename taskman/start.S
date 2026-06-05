/*
 * taskman — entry point.
 *
 * The seL4 kernel jumps here in U-mode after rootserver setup.
 * Registers on entry (per seL4 ABI):
 *   a0 = seL4_BootInfo * (virtual address of the bootinfo frame)
 *
 * We pass a0 straight through as the first argument to main().
 */

.section .text.start
.global _start
_start:
    /* Initialise the RISC-V global pointer (gp). The compiler uses
     * gp-relative addressing for small data/bss (-msmall-data limit
     * defaults to 8). __global_pointer$ is supplied by the linker and
     * points to the middle of .sdata. The .option norelax stanza
     * prevents the assembler from relaxing this load into a gp-relative
     * addressing (which would be circular). */
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    /* Plant tp = &qsoe_main_tcb so libqsoe's per-thread state
     * accessors (qsoe_curthr() in <qsoe/tls.h>) resolve. qsoe_main_tcb
     * is BSS, so any .ipcbuf/.qerrno reads before qsoe_libc_init()
     * see zeros, which is the correct uninitialised state. */
    la tp, qsoe_main_tcb

    /* Set up stack pointer. */
    la sp, _stack_top

    /* Clear frame pointer (no caller). */
    li fp, 0

    /* a0 already holds the bootinfo pointer — leave it for main(). */
    call main

    /* main returned — spin (wfi traps in U-mode on RISC-V). */
1:  j 1b

    .size _start, . - _start

.section .bss
    .align 12
_stack_bottom:
    .skip 8192
_stack_top:
