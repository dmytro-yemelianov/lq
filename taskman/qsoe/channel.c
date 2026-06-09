/*
 * libqsoe/src/channel.c — ChannelCreate / ChannelDestroy.
 *
 * IN_TASKMAN build  : direct call into the local tm_* handler.
 * Standalone build  : seL4_Call to taskman via QSOE_CAP_TASKMAN_EP,
 *                     with the wire protocol defined in <qsoe/wire.h>.
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#  include "proc/proc.h"

int ChannelCreate(unsigned flags)
{
    int chid = qsoe_state_alloc_chid(flags);
    if (chid < 0) { qsoe_errno = ENOMEM; return -1; }

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
}

int ChannelDestroy(int chid)
{
    unsigned long recv_slot = qsoe_state_chid_to_slot(chid);
    if (recv_slot == 0) { qsoe_errno = EBADF; return -1; }

    int err = tm_channel_destroy(QSOE_PID_TASKMAN, recv_slot);
    if (err != 0) { qsoe_errno = -err; return -1; }
    qsoe_state_bind_chid(chid, 0);
    return 0;
}
