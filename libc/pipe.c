/*
 * pipe.c — POSIX pipe().
 *
 * Asks taskman to mint a read/write cap pair onto /sbin/pipe's
 * channel via TM_REQ_PIPE_CREATE, then binds the two slot CPtrs
 * as fds in libqsoe's fd table.  IO on those fds talks directly
 * to /sbin/pipe — taskman doesn't proxy the read/write path.
 *
 * After this call, `fd[0]` is the read end, `fd[1]` is the write
 * end (POSIX convention).  Closing either fd decrements that
 * end's count inside /sbin/pipe; once both counts hit zero the
 * pipe slot is recycled.
 */

#include <unistd.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

int pipe(int fd[2])
{
    if (!fd) { qsoe_errno = EFAULT; return -1; }

    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PIPE_CREATE, 0, 0, 0);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    unsigned long read_slot  = (unsigned long)mr0;
    unsigned long write_slot = (unsigned long)mr1;

    int rfd = qsoe_state_alloc_coid(0);
    if (rfd < 0) { qsoe_errno = ENOMEM; return -1; }
    int wfd = qsoe_state_alloc_coid(0);
    if (wfd < 0) {
        qsoe_state_bind_coid(rfd, 0);
        qsoe_errno = ENOMEM;
        return -1;
    }
    qsoe_state_bind_coid(rfd, read_slot);
    qsoe_state_bind_coid(wfd, write_slot);

    fd[0] = rfd;
    fd[1] = wfd;
    return 0;
}
