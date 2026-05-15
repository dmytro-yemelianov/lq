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
 *
 * v0.5.0: also binds fds 0/1/2 to the stdio connections that spawn.c
 * minted into QSOE_CAP_STDIN/OUT/ERR_CONNECT at process creation.
 */

#include "../include/qsoe/qrv.h"
#include "../include/qsoe/slots.h"
#include "state.h"

extern int main(int argc, char **argv, char **envp);

/* musl's "syscall" indirection target. The patched syscall_arch.h
 * routes every musl libc syscall through this global function pointer
 * instead of an ecall. We assign it below before main() runs so by
 * the time printf / write / open are called, the dispatcher is wired
 * up. Defined as `size_t __sysinfo;` in musl's defsysinfo.c, pulled
 * into the link via the chain of musl uses. */
extern unsigned long __sysinfo;
extern long qsoe_syscall_dispatch(long n, long a, long b, long c,
                                  long d, long e, long f);

void _qsoe_start_main(pid_t pid, int argc, char **argv, char **envp);
void _qsoe_start_main(pid_t pid, int argc, char **argv, char **envp)
{
    qsoe_libqsoe_init((void *)0x1FE000UL, pid);

    /* v0.5.1: wire musl's "syscall" function pointer to libqsoe's
     * dispatcher BEFORE the user's main runs. After this, every
     * musl libc call (printf, fopen, ...) routes through QSOE. */
    __sysinfo = (unsigned long)qsoe_syscall_dispatch;

    /* v0.5.0: pre-bind stdio fds (0/1/2) to the console connections
     * taskman minted into our CSpace at spawn time. After this,
     * write(1, ...) / write(2, ...) "just work" without any explicit
     * open() — same shape as POSIX fd inheritance across exec. */
    qsoe_state_force_bind_coid(0, QSOE_CAP_STDIN_CONNECT);
    qsoe_state_force_bind_coid(1, QSOE_CAP_STDOUT_CONNECT);
    qsoe_state_force_bind_coid(2, QSOE_CAP_STDERR_CONNECT);

    /* v0.6.4: bring up the signal thread BEFORE main() so signal
     * delivery is live for the duration of the program.  Failure
     * here is non-fatal — main() runs without signal delivery. */
    extern int qsoe_signal_init(void);
    (void)qsoe_signal_init();

    int rc = main(argc, argv, envp);
    _exit(rc);
}
