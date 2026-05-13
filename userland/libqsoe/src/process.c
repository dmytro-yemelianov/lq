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
#  include "server.h"
#endif

/* Strlen for tiny C strings — libqsoe is freestanding, no libc. */
static unsigned qstrlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

int ProcessCreate(const char *path)
{
    if (!path) { qsoe_errno = EINVAL; return -1; }
    unsigned plen = qstrlen(path);
    if (plen == 0 || plen >= 64) { qsoe_errno = EINVAL; return -1; }

#ifdef QSOE_LIBQSOE_IN_TASKMAN
    pid_t new_pid = 0;
    int rc = tm_process_create_by_name(path, plen, &new_pid);
    if (rc) { qsoe_errno = -rc; return -1; }
    return (int)new_pid;
#else
    /* Pack the path bytes into the IPC buffer starting at msg[4]. MR0..3
     * (= ipcbuf->msg[0..3]) are register-transferred on the wire and
     * don't reach the receiver's ipcbuf in transit; the kernel only
     * copies msg[4..length-1] from sender's to receiver's ipcbuf. */
    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < plen; ++i) dst[i] = (unsigned char)path[i];

    seL4_Word mr0 = plen;
    seL4_Word mr1 = 0, mr2 = 0, mr3 = 0;
    /* Total words: 4 register-MRs + ceil(plen/8) words for the path. */
    unsigned nwords = 4 + (plen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PROCESS_CREATE,
                                                   0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return (int)mr0;
#endif
}

/* POSIX surface. v0.4.1 ignores everything but path/pid. */
int posix_spawn(pid_t *pid_out, const char *path,
                const void *file_actions,
                const void *attr,
                char *const argv[],
                char *const envp[])
{
    (void)file_actions; (void)attr; (void)argv; (void)envp;
    int p = ProcessCreate(path);
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
