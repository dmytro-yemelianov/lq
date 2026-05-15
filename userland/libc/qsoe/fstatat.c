/*
 * fstatat.c — POSIX fstatat() (the path-side stat family).
 *
 * Musl's stat() / lstat() funnel through this with dirfd=AT_FDCWD.
 * v0.7 strategy: open the path, fstat, close.  That reuses the
 * already-wired cpiofs/console stat handlers without a separate
 * path-side wire op.
 *
 * dirfd semantics:
 *   - absolute path: dirfd is ignored
 *   - relative path + AT_FDCWD: resolve against the caller's cwd
 *   - relative path + other fd: not yet supported — returns EBADF.
 *     Once cpiofs (or another resmgr) exposes per-fd path metadata
 *     this can be reimplemented properly.
 *
 * flags: AT_SYMLINK_NOFOLLOW is honored vacuously (no symlinks in
 * v0.7); other bits are ignored.
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stddef.h>
#include <qsoe-system.h>

int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    (void)flags;
    if (!path || !buf) { qsoe_errno = EFAULT; return -1; }

    /* Build the absolute path if the caller passed a relative one. */
    char abs[256];
    const char *target;
    if (path[0] == '/') {
        target = path;
    } else if (dirfd == AT_FDCWD) {
        /* cwd-relative: pull our cwd from taskman and concatenate. */
        if (!getcwd(abs, sizeof abs)) return -1;
        unsigned cwdlen = 0;
        while (abs[cwdlen] && cwdlen < sizeof abs) ++cwdlen;
        /* Add a trailing '/' if cwd != "/" itself. */
        if (cwdlen + 1 < sizeof abs && !(cwdlen == 1 && abs[0] == '/')) {
            abs[cwdlen++] = '/';
        }
        unsigned i = 0;
        while (path[i] && cwdlen + i + 1 < sizeof abs) {
            abs[cwdlen + i] = path[i];
            ++i;
        }
        if (path[i] != 0) { qsoe_errno = ENAMETOOLONG; return -1; }
        abs[cwdlen + i] = 0;
        target = abs;
    } else {
        /* Relative against an arbitrary dirfd not yet wired. */
        qsoe_errno = EBADF;
        return -1;
    }

    int fd = open(target, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fstat(fd, buf);
    int saved = qsoe_errno;
    close(fd);
    qsoe_errno = saved;
    return rc;
}
