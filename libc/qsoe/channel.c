/*
 * libqsoe/src/channel.c — ChannelCreate / ChannelDestroy.
 *
 * IN_TASKMAN build  : direct call into the local tm_* handler.
 * Standalone build  : seL4_Call to taskman via QSOE_CAP_TASKMAN_EP,
 *                     with the wire protocol defined in <qsoe/tm_msgs.h>.
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"


int ChannelCreate(unsigned flags)
{
    /* A global channel's chid is system-unique, so taskman assigns it (and
     * returns it below); a normal channel's chid is allocated here in the
     * per-process namespace. */
    int chid;
    if (flags & QSOE_CHF_GLOBAL) {
        chid = 0;
    } else {
        chid = qsoe_state_alloc_chid(flags);
        if (chid < 0) { qsoe_errno = ENOMEM; return -1; }
    }

    /* Wire: MR0 = suggested chid, MR1 = flags, MR2 = creating thread's tid
     * (taskman binds the channel's pulse notification to that thread -- the
     * receiver-by-convention).  Reply: label = errno, MR0 = recv slot,
     * MR1 = effective chid (taskman-assigned if global). */
    seL4_Word mr0 = (seL4_Word)chid;
    seL4_Word mr1 = (seL4_Word)flags;
    seL4_Word mr2 = (seL4_Word)qsoe_curthr()->tid;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_CHANNEL_CREATE, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) {
        if (!(flags & QSOE_CHF_GLOBAL)) qsoe_state_bind_chid(chid, 0);
        qsoe_errno = (int)err;
        return -1;
    }
    int eff_chid = (int)mr1;          /* assigned global chid, or echo */
    qsoe_state_bind_chid(eff_chid, mr0 /* recv slot */);
    return eff_chid;
}

int ChannelDestroy(int chid)
{
    unsigned long recv_slot = qsoe_state_chid_to_slot(chid);
    if (recv_slot == 0) { qsoe_errno = EBADF; return -1; }

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
}
