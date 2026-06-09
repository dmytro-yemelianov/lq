/*
 * devnull.c — /dev/null resource manager (v0.8+).
 *
 * The universal sink.  Reads return 0 (EOF); writes claim to have
 * consumed every byte and discard them.  No state.
 *
 * Hosted inside taskman like /dev/console — TM_DEVNULL_CHID shares
 * taskman's primary endpoint; path/io.c routes IO traffic by chid.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "devnull.h"
#include <sys/qsoe.h>

unsigned tm_devnull_write(unsigned nbytes)
{
    /* Pretend we wrote it all; ignore the bytes. */
    return nbytes;
}

int tm_devnull_read(unsigned want, unsigned *out_got)
{
    (void)want;
    *out_got = 0;   /* Immediate EOF. */
    return 0;
}

int tm_devnull_stat(tm_stat_t *out)
{
    if (!out) return -EINVAL;
    unsigned char *p = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof *out; ++i) p[i] = 0;
    out->st_dev     = 1;
    out->st_ino     = 3;
    out->st_mode    = TM_S_IFCHR | 0666;
    out->st_nlink   = 1;
    out->st_rdev    = (1UL << 8) | 3;
    out->st_blksize = 4096;
    return 0;
}
