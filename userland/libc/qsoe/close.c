/*
 * close.c — POSIX close() for QSOE.
 *
 * Wire-level half of taskman's resource-manager protocol: send
 * TM_REQ_CLOSE on the slot bound to the fd, drop the binding.
 *
 * Body moved verbatim from v0.6.4's libqsoe/src/io.c qsoe_close().
 */

#include <qsoe/qrv.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

int close(int fd);
int close(int fd)
{
    seL4_CPtr slot = (seL4_CPtr)qsoe_state_coid_to_slot(fd);
    if (!slot) { qsoe_errno = EBADF; return -1; }

    seL4_Word mr0 = (seL4_Word)slot, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_CLOSE, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    qsoe_state_bind_coid(fd, 0);
    return 0;
}
