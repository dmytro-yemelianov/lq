/*
 * libqsoe/src/channel.c — ChannelCreate / ChannelDestroy.
 *
 * IN_TASKMAN build  : direct call into the local tm_* handler.
 * Standalone build  : seL4_Call to taskman via QSOE_CAP_TASKMAN_EP,
 *                     with the wire protocol defined in <qsoe/wire.h>.
 */

#include <qsoe-system.h>
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#ifdef QSOE_LIBQSOE_IN_TASKMAN
#  include "proc/proc.h"
#endif

int ChannelCreate(unsigned flags)
{
    int chid = qsoe_state_alloc_chid(flags);
    if (chid < 0) { qsoe_errno = ENOMEM; return -1; }

#ifdef QSOE_LIBQSOE_IN_TASKMAN
    unsigned long recv_slot = 0;
    int err = tm_channel_create(QSOE_PID_TASKMAN, chid, flags,
                                (unsigned long *)&recv_slot);
    if (err != 0) {
        qsoe_state_bind_chid(chid, 0);
        qsoe_errno = -err;
        return -1;
    }
    qsoe_state_bind_chid(chid, recv_slot);
    return chid;
#else
    /* Wire: MR0 = chid, MR1 = flags. Reply: label = errno, MR0 = recv slot. */
    seL4_Word mr0 = (seL4_Word)chid;
    seL4_Word mr1 = (seL4_Word)flags;
    seL4_Word mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_CHANNEL_CREATE, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) {
        qsoe_state_bind_chid(chid, 0);
        qsoe_errno = (int)err;
        return -1;
    }
    qsoe_state_bind_chid(chid, mr0);
    return chid;
#endif
}

int ChannelDestroy(int chid)
{
    unsigned long recv_slot = qsoe_state_chid_to_slot(chid);
    if (recv_slot == 0) { qsoe_errno = EBADF; return -1; }

#ifdef QSOE_LIBQSOE_IN_TASKMAN
    int err = tm_channel_destroy(QSOE_PID_TASKMAN, recv_slot);
    if (err != 0) { qsoe_errno = -err; return -1; }
    qsoe_state_bind_chid(chid, 0);
    return 0;
#else
    /* Wire: MR0 = recv_slot. Reply: label = errno. */
    seL4_Word mr0 = (seL4_Word)recv_slot;
    seL4_Word mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_CHANNEL_DESTROY, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    qsoe_state_bind_chid(chid, 0);
    return 0;
#endif
}
