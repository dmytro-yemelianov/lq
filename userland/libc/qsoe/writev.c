/*
 * writev.c — POSIX writev() for QSOE.
 *
 * Loop over the iovec array calling write() on each segment.  No
 * atomicity guarantee yet (real concurrent-safe writev arrives once
 * resmgrs support gather-IO over shared memory).
 *
 * Body moved verbatim from v0.6.4's libqsoe/src/io.c qsoe_writev(),
 * adapted to take POSIX `struct iovec` from <sys/uio.h>.
 */

#include <sys/uio.h>
#include <unistd.h>
#include <qsoe/qrv.h>

ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    if (iovcnt < 0) { qsoe_errno = EINVAL; return -1; }
    size_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len == 0) continue;
        ssize_t rc = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return total ? (ssize_t)total : -1;
        total += (size_t)rc;
        if ((size_t)rc < iov[i].iov_len) break;
    }
    return (ssize_t)total;
}
