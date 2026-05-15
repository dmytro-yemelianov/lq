/*
 * getpid.c — POSIX getpid().
 *
 * The caller's pid is fixed for the lifetime of the process and
 * already lives in libqsoe's per-thread TCB (planted at spawn time
 * by taskman, read by qsoe_curthr()->self_pid).  No IPC needed —
 * this is the only cred-flavoured getter that hits zero kernel
 * calls.  The other six (getppid/getuid/etc.) route through
 * qsoe_proc_self_info() and TM_REQ_PROC_SELF_INFO.
 */

#include <unistd.h>
#include <qsoe/tls.h>

pid_t getpid(void)
{
    return qsoe_curthr()->self_pid;
}
