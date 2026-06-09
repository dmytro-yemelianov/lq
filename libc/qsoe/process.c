/*
 * lq/libc/qsoe/process.c -- LQ process-control seam.
 *
 * Provides the per-kernel primitives that the shared libc body needs
 * to implement POSIX process control on QSOE/L:
 *
 *   ProcessCreate / ProcessTerminate -- QNX-shape spawn/destroy via
 *                                       seL4 IPC to taskman.
 *   _Exit                            -- shared exit()/->Exit() and
 *                                       _exit() bottom out here.
 *   wait4                            -- shared waitpid()/wait3() do.
 *
 * Everything else (exit, _exit, waitpid, posix_spawn, procmgr_detach)
 * comes from the shared body (libc/stdlib/, libc/1/, libc/qsoe/) and
 * routes through these four primitives.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <stdlib.h>

#include "sel4_types.h"
#include "qsoe_invoke.h"

static unsigned qstrlen(const char *s)
{
    unsigned n = 0;
    while (s && s[n]) ++n;
    return n;
}

/* Common spawn path used by both ProcessCreate and the shared body's
 * posix_spawn (which is implemented via ProcessCreate-with-args once
 * Stage-B lands; today the shared stub returns ENOSYS).
 *
 * Wire layout (TM_REQ_PROCESS_CREATE):
 *   msg[0]      = path bytes
 *   followed by argv[0..argc-1] bytes packed via the same shape
 *   followed by envp[0..envc-1] bytes
 */
static int process_create_with_args(const char *path,
                                    char *const argv[], int argc,
                                    char *const envp[], int envc)
{
    (void)argv; (void)argc; (void)envp; (void)envc;  /* v0.4.4 packing TODO */
    seL4_Word mr0 = (seL4_Word)(uintptr_t)path;
    seL4_Word mr1 = (seL4_Word)qstrlen(path);
    seL4_Word mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PROCESS_CREATE,
                                                   0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return (int)mr0;
}

int ProcessCreate(const char *path)
{
    char *argv[1];
    argv[0] = (char *)path;
    return process_create_with_args(path, argv, 1, 0, 0);
}

int ProcessTerminate(pid_t pid, int status)
{
    seL4_Word mr0 = (seL4_Word)pid;
    seL4_Word mr1 = (seL4_Word)status;
    seL4_Word mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PROCESS_TERMINATE,
                                                   0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    /* For self-terminate, this Call never returns (taskman revokes
     * our TCB inside the handler). */
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
}

/* C11 _Exit -- shared exit() and the musl 1/_exit.c wrapper bottom
 * out here.  Taskman destroys our TCB inside TM_REQ_PROCESS_TERMINATE
 * with pid=0; the spin guards against a hypothetical EBADF path. */
_Noreturn void _Exit(int status)
{
    ProcessTerminate(0, status);
    for (;;) __asm__ volatile("nop");
}

/* wait4 -- shared waitpid() bottoms out here.  rusage is currently
 * unused (taskman doesn't track per-process resource counters
 * separately from the global hwi blob). */
pid_t wait4(pid_t pid, int *status, int options, struct rusage *rusage)
{
    (void)options; (void)rusage;
    seL4_Word mr0 = (seL4_Word)pid, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_WAITPID, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    if (status) *status = (int)mr0;
    return (pid_t)pid;
}
