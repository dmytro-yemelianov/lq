/*
 * io.c — libqsoe's POSIX-y file-descriptor surface (v0.5+).
 *
 *   qsoe_open  / qsoe_close          — TM_REQ_OPEN  / TM_REQ_CLOSE
 *   qsoe_write / qsoe_writev         — TM_REQ_IO_WRITE on the fd's coid
 *   qsoe_read                        — TM_REQ_IO_READ on the fd's coid
 *
 * These functions are the wire-level half of taskman's resource-
 * manager protocol. musl libc's open() / write() / read() / close()
 * eventually route here through __sysinfo + qsoe_syscall_dispatch
 * (v0.5 Step 6).
 *
 * v0.5.0 carries write payloads inline in ipcbuf->msg[4..]; the
 * per-call cap is QSOE_IO_MAX_CHUNK bytes. qsoe_writev loops over
 * iovecs and chunks long writes. Real shared-memory or page-grant
 * paths come later when filesystem servers want to move pages, not
 * bytes.
 */

#include "../include/qsoe/qrv.h"
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

/* msg[4..119] = 116 words = 928 bytes. We round down conservatively. */
#define QSOE_IO_MAX_CHUNK  928u

static unsigned io_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

int qsoe_open(const char *path, int flags)
{
    (void)flags;  /* v0.5.0 ignores flags; v0.6+ propagates them */
    if (!path) { qsoe_errno = EINVAL; return -1; }
    unsigned plen = io_strlen(path);
    if (plen == 0 || plen >= 128) { qsoe_errno = EINVAL; return -1; }

    /* Pack the path into msg[4..] — same convention as
     * TM_REQ_PROCESS_CREATE; MR0..3 are register-passed and not
     * transferred via the ipc-buffer copy, so the bytes start at
     * msg[4]. */
    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < plen; ++i) dst[i] = (unsigned char)path[i];

    seL4_Word mr0 = plen, mr1 = 0, mr2 = 0, mr3 = 0;
    unsigned nwords = 4 + (plen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_OPEN, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    seL4_CPtr cap_slot = (seL4_CPtr)mr0;
    int fd = qsoe_state_alloc_coid(0);
    if (fd < 0) { qsoe_errno = ENOMEM; return -1; }
    qsoe_state_bind_coid(fd, cap_slot);
    return fd;
}

int qsoe_close(int fd)
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

/* Internal: send up to QSOE_IO_MAX_CHUNK bytes on `slot`. Returns the
 * server's reported bytes_written or -errno. */
static long io_write_chunk(seL4_CPtr slot, const unsigned char *buf, unsigned n)
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

long qsoe_write(int fd, const void *buf, unsigned long count)
{
    seL4_CPtr slot = (seL4_CPtr)qsoe_state_coid_to_slot(fd);
    if (!slot) { qsoe_errno = EBADF; return -1; }

    const unsigned char *p = buf;
    unsigned long total = 0;
    while (count > 0) {
        unsigned chunk = (count > QSOE_IO_MAX_CHUNK) ? QSOE_IO_MAX_CHUNK
                                                     : (unsigned)count;
        long rc = io_write_chunk(slot, p, chunk);
        if (rc < 0) return total ? (long)total : -1;
        total += (unsigned long)rc;
        p     += rc;
        count -= (unsigned long)rc;
        /* Short writes shouldn't happen on the console resmgr, but
         * stop the loop if they do so we don't spin. */
        if ((unsigned)rc < chunk) break;
    }
    return (long)total;
}

long qsoe_read(int fd, void *buf, unsigned long count)
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
    return (long)got;
}

struct qsoe_iovec {
    void         *iov_base;
    unsigned long iov_len;
};

long qsoe_writev(int fd, const struct qsoe_iovec *iov, int iovcnt)
{
    if (iovcnt < 0) { qsoe_errno = EINVAL; return -1; }
    unsigned long total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len == 0) continue;
        long rc = qsoe_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return total ? (long)total : -1;
        total += (unsigned long)rc;
        if ((unsigned long)rc < iov[i].iov_len) break;
    }
    return (long)total;
}
