/*
 * proc_info.c — wire wrapper for TM_REQ_PROC_SELF_INFO.
 *
 * Backs POSIX getpid / getppid / getuid / geteuid / getgid / getegid.
 * Returns the caller's own (pid, ppid) and cred fields in a single
 * IPC round-trip; no per-getter IPC traffic.
 *
 * Reply layout matches taskman/main.c's TM_REQ_PROC_SELF_INFO case:
 *   mr0 = pid     | (ppid << 32)
 *   mr1 = ruid    | (euid << 32)
 *   mr2 = suid    | (rgid << 32)
 *   mr3 = egid    | (sgid << 32)
 */

#include <qsoe-system.h>
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

int qsoe_proc_self_info(pid_t *out_pid, pid_t *out_ppid,
                        struct _cred_info *out_cred);
int qsoe_proc_self_info(pid_t *out_pid, pid_t *out_ppid,
                        struct _cred_info *out_cred)
{
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PROC_SELF_INFO,
                                                   0, 0, 0);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    if (out_pid)  *out_pid  = (pid_t)(mr0 & 0xFFFFFFFFu);
    if (out_ppid) *out_ppid = (pid_t)((mr0 >> 32) & 0xFFFFFFFFu);
    if (out_cred) {
        out_cred->ruid    = (uid_t)(mr1 & 0xFFFFFFFFu);
        out_cred->euid    = (uid_t)((mr1 >> 32) & 0xFFFFFFFFu);
        out_cred->suid    = (uid_t)(mr2 & 0xFFFFFFFFu);
        out_cred->rgid    = (gid_t)((mr2 >> 32) & 0xFFFFFFFFu);
        out_cred->egid    = (gid_t)(mr3 & 0xFFFFFFFFu);
        out_cred->sgid    = (gid_t)((mr3 >> 32) & 0xFFFFFFFFu);
        out_cred->ngroups = 0;
    }
    return 0;
}
