/*
 * errno.c — QSOE-native __errno_location().
 *
 * Replaces upstream musl's src/errno/__errno_location.c, which reads
 * the per-thread errno via __pthread_self()->errno_val and so needs
 * pthread_impl.h (which itself drags SYS_futex).
 *
 * libqsoe already maintains a per-thread errno cell at
 * qsoe_curthr()->qerrno (exposed to libqsoe callers as `qsoe_errno`
 * via tls.h's macro).  Hooking libc's errno macro to the same cell
 * means a QSOE POSIX entry point that sets qsoe_errno and a musl-side
 * caller that reads errno end up seeing the same value automatically.
 */

#include <qsoe/tls.h>

int *__errno_location(void);
int *__errno_location(void)
{
    return &qsoe_curthr()->qerrno;
}

/* musl-internal sources include src/include/errno.h, which routes
 * the errno macro through `___errno_location` (triple underscore,
 * declared hidden) so it cannot be hijacked from outside the library.
 * Alias it to our single per-thread implementation. */
int *___errno_location(void) __attribute__((alias("__errno_location")));
