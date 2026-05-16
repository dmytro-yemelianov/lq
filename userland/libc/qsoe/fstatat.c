/*
 * fstatat.c — POSIX fstatat() (the path-side stat family).
 *
 * Musl's stat() / lstat() funnel through this with dirfd=AT_FDCWD.
 * v0.7 strategy: open the path, fstat, close.  That reuses the
 * already-wired cpiofs/console stat handlers without a separate
 * path-side wire op.
 *
 * Path resolution (cwd prepend + . / .. / // canonicalisation) lives
 * in open() — see userland/libc/qsoe/open.c.  This file just calls
 * open() with whatever the caller passed; open() handles relative
 * paths and canonical forms uniformly.
 *
 * dirfd semantics:
 *   - absolute path: dirfd is ignored.
 *   - relative path + AT_FDCWD: open() resolves against the cwd.
 *   - relative path + other fd: not yet supported — returns EBADF.
 *     Lands when cpiofs (or another resmgr) exposes per-fd path
 *     metadata.
 *
 * flags: AT_SYMLINK_NOFOLLOW is honored vacuously (cpiofs symlinks
 * are followed at open time); other bits are ignored.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
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

    if (path[0] != '/' && dirfd != AT_FDCWD) {
        /* dirfd-relative-not-AT_FDCWD lands when a future resmgr
         * carries per-fd path metadata; for now, refuse. */
        qsoe_errno = EBADF;
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = fstat(fd, buf);
    int saved = qsoe_errno;
    close(fd);
    qsoe_errno = saved;
    return rc;
}
