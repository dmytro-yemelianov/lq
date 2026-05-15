/*
 * read.c — POSIX read() for QSOE.
 *
 * TM_REQ_IO_READ on the fd's coid; payload returned inline in
 * msg[4..] (capped at QSOE_IO_MAX_CHUNK bytes per call).
 *
 * Body moved verbatim from v0.6.4's libqsoe/src/io.c qsoe_read().
 */

#include <unistd.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

/* msg[4..119] = 116 words = 928 bytes.  Conservative round-down. */
#define QSOE_IO_MAX_CHUNK  928u

ssize_t read(int fd, void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count)
{
    seL4_CPtr slot = (seL4_CPtr)qsoe_state_coid_to_slot(fd);
    if (!slot) { qsoe_errno = EBADF; return -1; }
    if (count > QSOE_IO_MAX_CHUNK) count = QSOE_IO_MAX_CHUNK;

    seL4_Word mr0 = (seL4_Word)count, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_IO_READ, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(slot, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    unsigned got = (unsigned)mr0;
    if (got > count) got = (unsigned)count;
    unsigned char *dst = buf;
    unsigned char *src = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < got; ++i) dst[i] = src[i];
    return (ssize_t)got;
}
