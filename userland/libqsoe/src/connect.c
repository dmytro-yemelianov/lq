/*
 * libqsoe/src/connect.c — ConnectAttach / ConnectDetach.
 *
 * IN_TASKMAN build : direct call into tm_*.
 * Standalone       : seL4_Call to taskman via QSOE_CAP_TASKMAN_EP.
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

int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags)
{
    (void)index;
    if (nd != ND_LOCAL_NODE) { qsoe_errno = EHOSTUNREACH; return -1; }

    int coid = qsoe_state_alloc_coid();
    if (coid < 0) { qsoe_errno = ENOMEM; return -1; }

#ifdef QSOE_LIBQSOE_IN_TASKMAN
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
    /* Wire: MR0 = pid, MR1 = chid, MR2 = flags. Reply: label=errno, MR0=send slot. */
    seL4_Word mr0 = (seL4_Word)pid;
    seL4_Word mr1 = (seL4_Word)chid;
    seL4_Word mr2 = (seL4_Word)flags;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_CONNECT_ATTACH, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) {
        qsoe_state_bind_coid(coid, 0);
        qsoe_errno = (int)err;
        return -1;
    }
    qsoe_state_bind_coid(coid, mr0);
    return coid;
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
    seL4_Word mr0 = (seL4_Word)send_slot;
    seL4_Word mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_CONNECT_DETACH, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    qsoe_state_bind_coid(coid, 0);
    return 0;
#endif
}
