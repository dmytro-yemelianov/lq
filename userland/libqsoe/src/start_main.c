/*
 * start_main.c — shared C entry shim for spawned QSOE binaries.
 *
 * The crt0 in each binary's start.S sets up gp, tp, and the
 * SysV-ABI initial-stack layout's arguments in registers, then jumps
 * here. We do the libqsoe init (which needs to happen before any
 * libqsoe call) and then dispatch to the user's main, finally
 * funnelling the return value into _exit so taskman sees a
 * ProcessTerminate.
 *
 * The kernel-supplied a0=pid (set by taskman's TCB_WriteRegisters in
 * spawn.c) is the first argument; the remaining three came off the
 * top of the initial stack per RISC-V SysV ABI.
 */

#include "../include/qsoe/qrv.h"

extern int main(int argc, char **argv, char **envp);

void _qsoe_start_main(pid_t pid, int argc, char **argv, char **envp);
void _qsoe_start_main(pid_t pid, int argc, char **argv, char **envp)
{
    qsoe_libqsoe_init((void *)0x1FE000UL, pid);
    int rc = main(argc, argv, envp);
    _exit(rc);
}
