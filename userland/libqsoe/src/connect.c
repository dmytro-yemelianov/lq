/*
 * libqsoe/src/connect.c — ConnectAttach / ConnectDetach.
 *
 * v0.2 IN_TASKMAN path only; see channel.c comment.
 */

#include "../include/qsoe/qrv.h"
#include "../include/qsoe/slots.h"
#include "state.h"

#ifdef QSOE_LIBQSOE_IN_TASKMAN
#  include "server.h"
#endif

int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags)
{
    (void)index;
    if (nd != ND_LOCAL_NODE) { qsoe_errno = EHOSTUNREACH; return -1; }

#ifdef QSOE_LIBQSOE_IN_TASKMAN
    int coid = qsoe_state_alloc_coid();
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
#else
    (void)pid; (void)chid; (void)flags;
    qsoe_errno = EINVAL;
    return -1;
#endif
}

int ConnectDetach(int coid)
{
    unsigned long send_slot = qsoe_state_coid_to_slot(coid);
    if (send_slot == 0) { qsoe_errno = EBADF; return -1; }

#ifdef QSOE_LIBQSOE_IN_TASKMAN
    int err = tm_connect_detach(QSOE_PID_TASKMAN, send_slot);
    if (err != 0) { qsoe_errno = -err; return -1; }
    qsoe_state_bind_coid(coid, 0);
    return 0;
#else
    qsoe_errno = EINVAL;
    return -1;
#endif
}
