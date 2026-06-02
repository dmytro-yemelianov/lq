/*
 * libqsoe/src/connect.c — ConnectAttach / ConnectDetach.
 *
 * IN_TASKMAN build : direct call into tm_*.
 * Standalone       : seL4_Call to taskman via QSOE_CAP_TASKMAN_EP.
 */

#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#  include "proc/proc.h"

int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags)
{
    (void)index;
    if (nd != ND_LOCAL_NODE) { qsoe_errno = EHOSTUNREACH; return -1; }

    int coid = qsoe_state_alloc_coid((unsigned)flags);
    if (coid < 0) { qsoe_errno = ENOMEM; return -1; }

    unsigned long send_slot = 0;
    int err = tm_connect_attach(QSOE_PID_TASKMAN, pid, chid, (unsigned)flags,
                                (unsigned long *)&send_slot);
    if (err != 0) {
        qsoe_state_bind_coid(coid, 0);
        qsoe_errno = -err;
        return -1;
    }
    qsoe_state_bind_coid(coid, send_slot);
    return coid;
}

int ConnectDetach(int coid)
{
    unsigned long send_slot = qsoe_state_coid_to_slot(coid);
    if (send_slot == 0) { qsoe_errno = EBADF; return -1; }

    int err = tm_connect_detach(QSOE_PID_TASKMAN, send_slot);
    if (err != 0) { qsoe_errno = -err; return -1; }
    qsoe_state_bind_coid(coid, 0);
    return 0;
}

/* ---------------- v0.3.3 introspection / control ---------------- */

/* Resolve `pid` to the effective caller pid for "self-only" queries.
 * QNX convention: pid==0 means "this process". Anything else in v0.3.3
 * returns ENOSYS — querying another process's coid table needs cross-
 * process state we don't have yet. */
static int self_only(pid_t pid)
{
    return (pid == 0 || pid == qsoe_self_pid);
}

int ConnectServerInfo(pid_t pid, int coid, struct _server_info *info)
{
    if (!info) { qsoe_errno = EINVAL; return -1; }
    if (!self_only(pid)) { qsoe_errno = ENOSYS; return -1; }

    unsigned long slot = qsoe_state_coid_to_slot(coid);
    if (slot == 0) { qsoe_errno = EBADF; return -1; }

    pid_t     server_pid = 0;
    int       server_chid = 0;
    seL4_Word scoid = 0;

    int err = tm_connect_server_info(qsoe_self_pid, (seL4_CPtr)slot,
                                      &server_pid, &server_chid, &scoid);
    if (err) { qsoe_errno = -err; return -1; }

    info->nd        = ND_LOCAL_NODE;
    info->pid       = server_pid;
    info->chid      = server_chid;
    info->scoid     = (int)scoid;
    info->coid      = coid;
    info->msglen    = 0;
    info->srcmsglen = 0;
    info->dstmsglen = 0;
    info->priority  = 0;
    info->flags     = 0;
    return 0;
}

int ConnectClientInfo(int scoid, struct _client_info *info, int ngroups)
{
    (void)ngroups;  /* no group lists in v0.3.3 */
    if (!info) { qsoe_errno = EINVAL; return -1; }

    pid_t    client_pid = 0;
    pid_t    sid = 0;
    unsigned flags = 0;

    int err = tm_connect_client_info((seL4_Word)scoid,
                                      &client_pid, &sid, &flags);
    if (err) { qsoe_errno = -err; return -1; }

    info->nd  = ND_LOCAL_NODE;
    info->pid = client_pid;
    info->sid = sid;
    info->flags = flags;
    info->cred.ruid = 0; info->cred.euid = 0; info->cred.suid = 0;
    info->cred.rgid = 0; info->cred.egid = 0; info->cred.sgid = 0;
    info->cred.ngroups = 0;
    return 0;
}

int ConnectFlags(pid_t pid, int coid, unsigned mask, unsigned bits)
{
    if (!self_only(pid)) { qsoe_errno = ENOSYS; return -1; }

    unsigned long slot = qsoe_state_coid_to_slot(coid);
    if (slot == 0) { qsoe_errno = EBADF; return -1; }

    unsigned old = 0;
    int err = tm_connect_flags(qsoe_self_pid, (seL4_CPtr)slot,
                                mask, bits, &old);
    if (err) { qsoe_errno = -err; return -1; }
    return (int)old;
}
