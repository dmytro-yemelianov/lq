/*
 * path/sysfs.c -- LQ wiring for the synthetic read-only /sys tree.
 *
 * Glue between the OS-independent /sys model (libtaskman <tm_sysfs.h>)
 * and LQ's in-taskman resmgr plumbing.  tm_sysfs_populate() sources the
 * file contents from the syscfg blob (board = root /compatible) plus the
 * generated version header; the per-fd handlers below are dispatched by
 * path/io.c on (taskman, TM_SYSFS_CHID) and keep a small per-open slot
 * pool keyed by connection badge, mirroring pmdir.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sysfs.h"
#include "../sys/syscfg.h"
#include "../proc/proc.h"         /* tm_connection_{set,get}_ctx */
#include <tm_sysfs.h>
#include <qsoe/ipcbuf.h>
#include <qsoe/syscfg.h>          /* TM_SYSCFG_TAG_COMPATIBLE / _MODEL */
#include <qsoe/sys_version.h>     /* QSOE_VERSION_STRING / QSOE_BUILD_DATE */

/* ---- Content population -------------------------------------------- */

void tm_sysfs_populate(void)
{
    /* board -- the root FDT /compatible string, emitted into the syscfg
     * blob by lq/taskman/sys/syscfg.c.  Fall back to the machine model,
     * then to "unknown" so init's `read < /sys/board` always gets a
     * line. */
    const void *board = 0;
    unsigned    blen  = 0;
    if (tm_syscfg_find(TM_SYSCFG_TAG_COMPATIBLE, &board, &blen) != 0 ||
        board == 0 || blen == 0) {
        if (tm_syscfg_find(TM_SYSCFG_TAG_MODEL, &board, &blen) != 0)
            board = 0;
    }

    /* cmdline -- LQ has no /chosen/bootargs tag in syscfg yet; pass NULL
     * so /sys/cmdline reads as an empty line until the boot cmdline is
     * plumbed into syscfg. */
    tm_sysfs_init((const char *)board, /*cmdline=*/0,
                  QSOE_VERSION_STRING, QSOE_BUILD_DATE);
}

/* ---- Per-open state -------------------------------------------------
 *
 * Stored in the CONNECTION ctx (tm_connection_{set,get}_ctx), exactly
 * like cpiofs/pmdir -- NOT a local badge-keyed table.  This matters:
 * `read X < /sys/board` opens then dup2()s the fd onto stdin, and
 * tm_dup_cap clones the connection (copying its ctx) under a fresh
 * badge.  Per-fd state kept in the ctx rides along; state kept in a
 * local pool keyed by the original badge would be lost (read EBADF).
 *
 * Encoding: ctx[0] == 0 marks the /sys directory; otherwise it is the
 * file's (entry index + 1).  ctx[1] is the read / readdir cursor. */

int tm_sysfs_open(const char *path, seL4_Word badge)
{
    unsigned idx = 0;
    int k = tm_sysfs_resolve(path, &idx);
    if (k == 0) return -ENOENT;
    unsigned long ctx0 = (k == 1) ? 0UL : (unsigned long)(idx + 1);
    return tm_connection_set_ctx(badge, ctx0, 0);
}

int tm_sysfs_read(seL4_Word badge, unsigned want, unsigned *out_got)
{
    *out_got = 0;
    unsigned long ctx0 = 0, ctx1 = 0;
    if (tm_connection_get_ctx(badge, &ctx0, &ctx1) != 0) return -EBADF;
    if (ctx0 == 0) return -EISDIR;

    unsigned len = 0;
    const char *data = tm_sysfs_content((unsigned)(ctx0 - 1), &len);
    if (data == 0) return -EBADF;

    unsigned long pos = ctx1;
    unsigned remain = (pos < len) ? (unsigned)(len - pos) : 0;
    unsigned n = (want < remain) ? want : remain;

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < n; ++i) dst[i] = (unsigned char)data[pos + i];
    *out_got = n;
    return tm_connection_set_ctx(badge, ctx0, pos + n);
}

int tm_sysfs_readdir(seL4_Word badge, char *name_out,
                     unsigned *namelen_out, int *d_type_out)
{
    *namelen_out = 0;
    unsigned long ctx0 = 0, ctx1 = 0;
    if (tm_connection_get_ctx(badge, &ctx0, &ctx1) != 0) return -EBADF;
    if (ctx0 != 0) return -ENOTDIR;

    if (ctx1 >= tm_sysfs_nentries()) return -ENOENT;     /* end of dir */

    const char *name = tm_sysfs_entry_name((unsigned)ctx1);
    unsigned namelen = 0;
    while (name[namelen] != '\0') namelen++;
    for (unsigned i = 0; i < namelen; ++i) name_out[i] = name[i];

    *namelen_out = namelen;
    *d_type_out  = 8;        /* DT_REG -- every /sys entry is a file */
    return tm_connection_set_ctx(badge, 0, ctx1 + 1);
}

int tm_sysfs_fstat(seL4_Word badge, tm_stat_t *out)
{
    unsigned long ctx0 = 0, ctx1 = 0;
    if (tm_connection_get_ctx(badge, &ctx0, &ctx1) != 0) return -EBADF;

    unsigned char *p = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof *out; ++i) p[i] = 0;
    out->st_blksize = 4096;

    if (ctx0 == 0) {
        out->st_dev   = 1;
        out->st_ino   = 17;                  /* synthetic */
        out->st_mode  = TM_S_IFDIR | 0555;
        out->st_nlink = 2;
    } else {
        unsigned len = 0;
        (void)tm_sysfs_content((unsigned)(ctx0 - 1), &len);
        out->st_dev   = 1;
        out->st_ino   = 17 + ctx0;
        out->st_mode  = TM_S_IFREG | 0444;
        out->st_nlink = 1;
        out->st_size  = (long)len;
    }
    return 0;
}

int tm_sysfs_lseek(seL4_Word badge, int whence, long offset, long *out_off)
{
    unsigned long ctx0 = 0, ctx1 = 0;
    if (tm_connection_get_ctx(badge, &ctx0, &ctx1) != 0) return -EBADF;
    if (ctx0 == 0) return -EISDIR;           /* dir cursor advances via readdir */

    unsigned len = 0;
    (void)tm_sysfs_content((unsigned)(ctx0 - 1), &len);

    long newpos;
    switch (whence) {
    case 0: newpos = offset;                       break;  /* SEEK_SET */
    case 1: newpos = (long)ctx1 + offset;          break;  /* SEEK_CUR */
    case 2: newpos = (long)len + offset;           break;  /* SEEK_END */
    default: return -EINVAL;
    }
    if (newpos < 0) return -EINVAL;
    *out_off = newpos;
    return tm_connection_set_ctx(badge, ctx0, (unsigned long)newpos);
}

int tm_sysfs_close(seL4_Word badge)
{
    /* No external state: the connection ctx is reclaimed when the
     * connection is detached.  Nothing to free here. */
    (void)badge;
    return 0;
}
