/*
 * lseek.c — POSIX lseek().
 *
 * Routes via the fd's bound connection so taskman dispatches to the
 * right resmgr.  cpiofs files are seekable (offset tracked in the
 * connection's ctx); console / pipes return ESPIPE.
 *
 * Defines both `lseek` (the POSIX name) and `__lseek` (musl's
 * internal weak alias used by stdio's __stdio_seek vtable hook).
 */

#include <unistd.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

off_t lseek(int fd, off_t offset, int whence)
{
    seL4_CPtr slot = (seL4_CPtr)qsoe_state_coid_to_slot(fd);
    if (!slot) { qsoe_errno = EBADF; return (off_t)-1; }

    seL4_Word mr0 = (seL4_Word)whence;
    seL4_Word mr1 = (seL4_Word)offset;
    seL4_Word mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_LSEEK, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(slot, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return (off_t)-1; }
    return (off_t)mr0;
}

off_t __lseek(int fd, off_t offset, int whence)
    __attribute__((alias("lseek")));
