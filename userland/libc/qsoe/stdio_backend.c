/*
 * stdio_backend.c — FILE vtable functions for stdin/stdout/stderr.
 *
 * Replaces the three upstream files we excluded with the rest of the
 * __syscall callers:
 *
 *   src/stdio/__stdio_close.c   — issued SYS_close on f->fd
 *   src/stdio/__stdout_write.c  — issued SYS_writev on f->fd
 *   src/stdio/__stdio_write.c   — same, the generic write path
 *
 * Plus `__lseek`, the internal-name alias the upstream lseek wrapper
 * provided (and which `__stdio_seek` calls).
 *
 * For QSOE these functions route through libqsoe's wire-level IO
 * helpers (qsoe_close / qsoe_write / qsoe_read) — same path that
 * served fds before the dispatcher removal, just reached directly
 * from libc.a instead of via __sysinfo.
 *
 * stdio_impl.h gives us the FILE struct layout.  We pull it in via
 * the libc/qsoe/syscall.h stub on the include path (stdio_impl.h
 * itself unconditionally does `#include "syscall.h"`).
 */

#include "stdio_impl.h"
#include <unistd.h>
#include <qsoe/qrv.h>

int __stdio_close(FILE *f)
{
    return close(f->fd);
}

/* No buffered iovec path yet — libqsoe's qsoe_write already chunks
 * large writes internally (QSOE_IO_MAX_CHUNK).  Calling that for both
 * the FILE's buffer-flush portion and the user's buf is correct;
 * stdio assumes a single `write` returns total bytes written. */
size_t __stdio_write(FILE *f, const unsigned char *buf, size_t len)
{
    /* Flush any data already in the FILE's wbuf, then the user data. */
    size_t buffered = (size_t)(f->wpos - f->wbase);
    if (buffered) {
        ssize_t rc = write(f->fd, f->wbase, buffered);
        if (rc < 0) {
            f->flags |= F_ERR;
            return 0;
        }
        /* Short writes shouldn't happen on a streaming fd; if they
         * do, surface as error rather than silently discarding. */
        if ((size_t)rc < buffered) {
            f->flags |= F_ERR;
            return 0;
        }
    }
    f->wend = f->buf + f->buf_size;
    f->wpos = f->wbase = f->buf;

    ssize_t rc = write(f->fd, buf, len);
    if (rc < 0) { f->flags |= F_ERR; return 0; }
    return (size_t)rc;
}

/* Upstream __stdout_write probes TIOCGWINSZ to decide line-buffering;
 * QSOE's console is always a tty for v0.6, so just enable line-buf
 * once and forward to __stdio_write. */
size_t __stdout_write(FILE *f, const unsigned char *buf, size_t len)
{
    f->write = __stdio_write;
    return __stdio_write(f, buf, len);
}

/* __lseek now lives in lseek.c (POSIX-real, routes through the fd's
 * resmgr); not duplicated here. */
