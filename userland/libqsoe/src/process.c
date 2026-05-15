/*
 * libqsoe/src/process.c — process lifecycle (v0.4.1).
 *
 *   ProcessCreate(path)            — QNX-style minimal spawner; returns pid
 *   posix_spawn(*pid, path, ...)   — POSIX wrapper around ProcessCreate
 *   exit(status), _exit(status)    — self-terminate via wire to taskman
 *
 * v0.4.1 ignores argv/envp/file_actions/attr — the child still gets just
 * its pid in a0 like in v0.3.x. Proper argv-on-the-child-stack delivery
 * is a small follow-up (v0.4.1 polish or 0.5).
 */

#include "../include/qsoe/qrv.h"
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#ifdef QSOE_LIBQSOE_IN_TASKMAN
#  include "proc/proc.h"
#endif

/* Strlen for tiny C strings — libqsoe is freestanding, no libc. */
static unsigned qstrlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

/* Internal: do a ProcessCreate with explicit argv/envp arrays. */
static int process_create_with_args(const char *path,
                                     char *const argv[], int argc,
                                     char *const envp[], int envc)
{
    if (!path) { qsoe_errno = EINVAL; return -1; }
    unsigned plen = qstrlen(path);
    if (plen == 0 || plen >= 64) { qsoe_errno = EINVAL; return -1; }
    if (argc < 0 || argc > 16 || envc < 0 || envc > 16) {
        qsoe_errno = EINVAL;
        return -1;
    }

#ifdef QSOE_LIBQSOE_IN_TASKMAN
    pid_t new_pid = 0;
    int rc = tm_process_create_by_name(path, plen,
                                        argc, (const char *const *)argv,
                                        envc, (const char *const *)envp,
                                        &new_pid);
    if (rc) { qsoe_errno = -rc; return -1; }
    return (int)new_pid;
#else
    /* Pack the path + all argv/envp strings into the IPC buffer
     * starting at msg[4]. Each string is NUL-terminated. The kernel
     * only transfers msg[4..length-1] across IPC; MR0..3 carry the
     * counts and total byte length. */
    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    unsigned off = 0;
    /* path. */
    for (unsigned i = 0; i < plen; ++i) dst[off++] = (unsigned char)path[i];
    dst[off++] = 0;
    /* argv strings. */
    for (int i = 0; i < argc; ++i) {
        unsigned len = qstrlen(argv[i]);
        for (unsigned j = 0; j < len; ++j) dst[off++] = (unsigned char)argv[i][j];
        dst[off++] = 0;
    }
    /* envp strings. */
    for (int i = 0; i < envc; ++i) {
        unsigned len = qstrlen(envp[i]);
        for (unsigned j = 0; j < len; ++j) dst[off++] = (unsigned char)envp[i][j];
        dst[off++] = 0;
    }

    seL4_Word mr0 = (seL4_Word)argc;
    seL4_Word mr1 = (seL4_Word)envc;
    seL4_Word mr2 = (seL4_Word)plen;
    seL4_Word mr3 = (seL4_Word)off;  /* total_strs_bytes */
    unsigned nwords = 4 + (off + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PROCESS_CREATE,
                                                   0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return (int)mr0;
#endif
}

int ProcessCreate(const char *path)
{
    /* v0.4.4: pass argv0 = path basename so the child sees argc>=1. */
    char *argv[1];
    argv[0] = (char *)path;
    return process_create_with_args(path, argv, 1, 0, 0);
}

/* POSIX surface. v0.4.4: argv/envp now flow through to the child. */
int posix_spawn(pid_t *pid_out, const char *path,
                const void *file_actions,
                const void *attr,
                char *const argv[],
                char *const envp[])
{
    (void)file_actions; (void)attr;

    /* Count argv / envp (POSIX: both arrays NULL-terminated). */
    int argc = 0;
    if (argv) while (argv[argc]) ++argc;
    int envc = 0;
    if (envp) while (envp[envc]) ++envc;

    int p = process_create_with_args(path, argv, argc, envp, envc);
    if (p < 0) return qsoe_errno;
    if (pid_out) *pid_out = (pid_t)p;
    return 0;
}

int ProcessTerminate(pid_t pid, int status)
{
#ifdef QSOE_LIBQSOE_IN_TASKMAN
    /* taskman should never call this on itself; it can on others. */
    if (pid == 0 || pid == QSOE_PID_TASKMAN) {
        qsoe_errno = EINVAL;
        return -1;
    }
    int rc = tm_process_terminate(pid, status);
    if (rc) { qsoe_errno = -rc; return -1; }
    return 0;
#else
    seL4_Word mr0 = (seL4_Word)pid;
    seL4_Word mr1 = (seL4_Word)status;
    seL4_Word mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PROCESS_TERMINATE,
                                                   0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    /* For self-terminate, this Call never returns (taskman revokes our
     * TCB inside the handler). If we do come back, it's an external
     * terminate that failed somewhere — surface the errno. */
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
#endif
}

void _exit(int status)
{
    /* Self-terminate via wire to taskman. This call doesn't return;
     * taskman revokes our TCB inside the handler. If we somehow do
     * come back (e.g. EBADF from taskman), spin so we don't fall
     * through to undefined territory. */
    ProcessTerminate(0, status);
    for (;;) __asm__ volatile("nop");
}

void exit(int status)
{
    /* No atexit/stdio teardown yet in v0.4.1 — go straight to _exit. */
    _exit(status);
}

/* v0.6.1: procmgr_detach — daemon's "I am ready" signal. taskman
 * delivers `status` to the parent's parked waitpid() and reparents
 * us to pid 1. Returns 0 on success, -1 on error. */
int procmgr_detach(int status)
{
#ifdef QSOE_LIBQSOE_IN_TASKMAN
    /* No-op for taskman itself — it has no parent in the QSOE model. */
    (void)status;
    return 0;
#else
    seL4_Word mr0 = (seL4_Word)status, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PROC_DETACH,
                                                   0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
#endif
}

/* v0.6.1: waitpid — block until `pid` detaches or exits, fill *status. */
int waitpid(pid_t pid, int *status, int options)
{
    (void)options;  /* WNOHANG and friends are v0.7+ */
#ifdef QSOE_LIBQSOE_IN_TASKMAN
    /* taskman itself doesn't waitpid anything. */
    (void)pid; (void)status;
    qsoe_errno = ENOSYS;
    return -1;
#else
    seL4_Word mr0 = (seL4_Word)pid, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_WAITPID, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    if (status) *status = (int)mr0;
    return (int)pid;
#endif
}
