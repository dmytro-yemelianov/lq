/*
 * dup2.c — POSIX dup2().
 *
 * Walk:
 *   1. Validate oldfd has a live cap binding; bail with EBADF if not.
 *   2. If newfd == oldfd, return newfd (POSIX no-op).
 *   3. Close newfd if it was already open (POSIX: silently replaces).
 *   4. Allocate a fresh empty slot in the process's own CSpace.
 *   5. Ask taskman to CNode_Copy oldfd's cap into the new slot
 *      (TM_REQ_DUP_CAP).
 *   6. Bind newfd to the new slot in libqsoe's fd table.
 *
 * After this each fd owns its own cap slot, so closing one doesn't
 * affect the other.
 */

#include <unistd.h>
#include <qsoe/qrv.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

int dup2(int oldfd, int newfd)
{
    unsigned long oldslot = qsoe_state_coid_to_slot(oldfd);
    if (!oldslot) { qsoe_errno = EBADF; return -1; }

    if (oldfd == newfd) return newfd;

    if (qsoe_state_coid_to_slot(newfd) != 0) {
        (void)close(newfd);  /* POSIX: ignore close errors for newfd */
    }

    unsigned long newslot = qsoe_state_alloc_empty_slot();
    if (!newslot) { qsoe_errno = ENOMEM; return -1; }

    seL4_Word mr0 = oldslot, mr1 = newslot, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_DUP_CAP, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) {
        qsoe_state_free_empty_slot(newslot);
        qsoe_errno = (int)err;
        return -1;
    }

    qsoe_state_force_bind_coid(newfd, newslot);
    return newfd;
}
