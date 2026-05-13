/*
 * libqsoe/src/channel.c — ChannelCreate / ChannelDestroy.
 *
 * For v0.2 only the QSOE_LIBQSOE_IN_TASKMAN build is exercised. The
 * non-IN_TASKMAN path (the real IPC to taskman) is stubbed; it lands
 * in v0.3 once we have a second process to call from.
 */

#include "../include/qsoe/qrv.h"
#include "../include/qsoe/slots.h"
#include "state.h"

#ifdef QSOE_LIBQSOE_IN_TASKMAN
#  include "server.h"
#endif

int ChannelCreate(unsigned flags)
{
#ifdef QSOE_LIBQSOE_IN_TASKMAN
    int chid = qsoe_state_alloc_chid();
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
#else
    (void)flags;
    qsoe_errno = EINVAL;
    return -1;
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
    qsoe_errno = EINVAL;
    return -1;
#endif
}
