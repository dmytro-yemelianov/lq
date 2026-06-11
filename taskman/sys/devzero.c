/*
 * devzero.c — /dev/zero resource manager (v0.8+).
 *
 * Inverse of /dev/null: reads always succeed with `want` zero
 * bytes; writes are discarded.  Used as a backing for anonymous
 * mmap, scratch buffers, and the occasional `dd if=/dev/zero ...`.
 *
 * Hosted inside taskman like /dev/null; TM_DEVZERO_CHID shares
 * taskman's primary endpoint and path/io.c demuxes by chid.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "devzero.h"
#include <sys/qsoe.h>
#include <qsoe/ipcbuf.h>

/* Bound by the IPC buffer payload area (msg[4..119] = 928 bytes).
 * Larger reads are chunked client-side by libc/qsoe's read.c. */
#define TM_DEVZERO_MAX_READ  928

unsigned tm_devzero_write(unsigned nbytes)
{
    return nbytes;   /* discard, pretend success */
}

int tm_devzero_read(unsigned want, unsigned *out_got)
{
    if (want > TM_DEVZERO_MAX_READ) want = TM_DEVZERO_MAX_READ;
    unsigned char *p = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < want; ++i) p[i] = 0;
    *out_got = want;
    return 0;
}

int tm_devzero_stat(tm_stat_t *out)
{
    if (!out) return -EINVAL;
    unsigned char *p = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof *out; ++i) p[i] = 0;
    out->st_dev     = 1;
    out->st_ino     = 5;
    out->st_mode    = TM_S_IFCHR | 0666;
    out->st_nlink   = 1;
    out->st_rdev    = (1UL << 8) | 5;
    out->st_blksize = 4096;
    return 0;
}
