/*
 * write.c — POSIX write() for QSOE.
 *
 * TM_REQ_IO_WRITE on the fd's coid; payload carried inline in
 * msg[4..] (capped at QSOE_IO_MAX_CHUNK per request).  Long writes
 * loop until done or a short write surfaces.
 *
 * Body moved verbatim from v0.6.4's libqsoe/src/io.c qsoe_write()
 * + the static helper io_write_chunk().
 *
 * Real shared-memory / page-grant paths will come later when
 * filesystem servers want to move pages instead of bytes.
 */

#include <unistd.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

/* msg[4..119] = 116 words = 928 bytes.  Conservative round-down. */
#define QSOE_IO_MAX_CHUNK  928u

/* Send up to QSOE_IO_MAX_CHUNK bytes on `slot`.  Returns the server's
 * reported bytes_written or -1 with qsoe_errno set. */
static long write_chunk(seL4_CPtr slot, const unsigned char *buf, unsigned n)
{
    if (n > QSOE_IO_MAX_CHUNK) n = QSOE_IO_MAX_CHUNK;
    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < n; ++i) dst[i] = buf[i];

    seL4_Word mr0 = n, mr1 = 0, mr2 = 0, mr3 = 0;
    unsigned nwords = 4 + (n + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_IO_WRITE, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(slot, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return (long)mr0;
}

ssize_t write(int fd, const void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count)
{
    seL4_CPtr slot = (seL4_CPtr)qsoe_state_coid_to_slot(fd);
    if (!slot) { qsoe_errno = EBADF; return -1; }

    const unsigned char *p = buf;
    size_t total = 0;
    while (count > 0) {
        unsigned chunk = (count > QSOE_IO_MAX_CHUNK) ? QSOE_IO_MAX_CHUNK
                                                     : (unsigned)count;
        long rc = write_chunk(slot, p, chunk);
        if (rc < 0) return total ? (ssize_t)total : -1;
        total += (size_t)rc;
        p     += rc;
        count -= (size_t)rc;
        /* Short writes shouldn't happen on the console resmgr, but
         * stop the loop if they do so we don't spin. */
        if ((unsigned)rc < chunk) break;
    }
    return (ssize_t)total;
}
