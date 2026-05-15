/*
 * close.c — POSIX close().
 *
 * Two-step close (v0.7+):
 *
 *   1. TM_REQ_CLOSE on the fd's own bound cap.  The cap points at
 *      whoever owns the file — taskman (for cpiofs / console) or
 *      an external resmgr (e.g. /sbin/pipe).  Either way, that
 *      resmgr gets a chance to decrement reader / writer counts,
 *      free per-fd state, etc.
 *   2. TM_REQ_DETACH_CAP to taskman's primary EP, which deletes the
 *      cap from this process's CSpace and frees the connection-table
 *      entry.  Without step 2 the cap would leak; without step 1 an
 *      external resmgr would never observe the close.
 *
 *   3. Unbind the libqsoe fd entry locally.
 *
 * The split makes external resmgrs (pipe / future fs servers) see
 * closes uniformly with the in-taskman ones — pipe-mgr's main loop
 * already handles TM_REQ_CLOSE; cpiofs's tm_cpiofs_close runs from
 * taskman's TM_REQ_CLOSE dispatch.
 */

#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

int close(int fd)
{
    seL4_CPtr slot = (seL4_CPtr)qsoe_state_coid_to_slot(fd);
    if (!slot) { qsoe_errno = EBADF; return -1; }

    /* Step 1: notify the resmgr by Calling on the fd's cap. */
    {
        seL4_Word a0 = 0, a1 = 0, a2 = 0, a3 = 0;
        seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_CLOSE, 0, 0, 0);
        seL4_MessageInfo_t reply = qsoe_sys_call(slot, tag,
                                                  &a0, &a1, &a2, &a3);
        seL4_Word err = seL4_MessageInfo_get_label(reply);
        /* A resmgr that doesn't recognise the close (e.g. very old
         * external server) replies with an error.  We still tear
         * down our side — the fd is going away regardless. */
        (void)err;
    }

    /* Step 2: ask taskman to delete the cap + drop the registry row. */
    {
        seL4_Word a0 = (seL4_Word)slot, a1 = 0, a2 = 0, a3 = 0;
        seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_DETACH_CAP,
                                                       0, 0, 1);
        seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                                  &a0, &a1, &a2, &a3);
        seL4_Word err = seL4_MessageInfo_get_label(reply);
        if (err != 0) { qsoe_errno = (int)err; return -1; }
    }

    /* Step 3: forget the fd locally. */
    qsoe_state_bind_coid(fd, 0);
    return 0;
}
