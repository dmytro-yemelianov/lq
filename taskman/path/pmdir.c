/*
 * path/pmdir.c — synthetic pathmgr-tree directory handler.
 *
 * See pmdir.h for the role.  Each opendir() on a PMDIR-registered
 * path consumes one slot here; the slot remembers the directory's
 * absolute path and the next child index.  readdir() walks
 * tm_pathmgr_child_at(path, idx, ...) once per call.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pmdir.h"
#include "pathmgr.h"
#include "../proc/proc.h"
#include "../tm_log.h"
#include <sys/qsoe.h>

#define TM_PMDIR_MAX_OPEN  8
#define TM_PMDIR_PATH_MAX  64

static struct {
    seL4_Word badge;                       /* 0 = unused */
    unsigned  next_idx;
    char      path[TM_PMDIR_PATH_MAX];
} g_slots[TM_PMDIR_MAX_OPEN];

static unsigned q_strlen(const char *s)
{
    unsigned n = 0; while (s[n]) ++n; return n;
}

int tm_pmdir_open(const char *path, seL4_Word badge)
{
    if (!path) return -EINVAL;
    unsigned plen = q_strlen(path);
    if (plen == 0 || plen >= TM_PMDIR_PATH_MAX) return -ENAMETOOLONG;

    for (int i = 0; i < TM_PMDIR_MAX_OPEN; ++i) {
        if (g_slots[i].badge != 0) continue;
        g_slots[i].badge    = badge;
        g_slots[i].next_idx = 0;
        for (unsigned k = 0; k <= plen; ++k) g_slots[i].path[k] = path[k];
        /* Store the dir-slot index in the connection's ctx so
         * readdir/close find it via tm_connection_get_ctx.  ctx[0]=0
         * marks "directory"; ctx[1] is our slot index. */
        return tm_connection_set_ctx(badge, 0, (unsigned long)i);
    }
    tm_warn("pmdir: open slot pool exhausted (path=%s)", path);
    return -ENOMEM;
}

int tm_pmdir_readdir(seL4_Word badge, char *name_out,
                     unsigned *namelen_out, int *d_type_out)
{
    *namelen_out = 0;

    unsigned long ctx0 = 0, ctx1 = 0;
    int rc = tm_connection_get_ctx(badge, &ctx0, &ctx1);
    if (rc) return rc;
    if (ctx0 != 0) return -ENOTDIR;        /* file-shaped ctx; wrong handler */

    int slot = (int)ctx1;
    if (slot < 0 || slot >= TM_PMDIR_MAX_OPEN) return -EBADF;
    if (g_slots[slot].badge != badge) return -EBADF;

    unsigned namelen = 0;
    rc = tm_pathmgr_child_at(g_slots[slot].path,
                              g_slots[slot].next_idx,
                              name_out, 256, &namelen);
    if (rc) return rc;
    g_slots[slot].next_idx++;
    *namelen_out = namelen;
    *d_type_out  = 8;       /* DT_REG — every leaf in pathmgr is a
                             * resmgr-served file/device today.  When
                             * nested PMDIR registrations land (e.g.
                             * /sys/proc), flip to DT_DIR for entries
                             * whose child has further children. */
    return 0;
}

int tm_pmdir_close(seL4_Word badge)
{
    unsigned long ctx0 = 0, ctx1 = 0;
    if (tm_connection_get_ctx(badge, &ctx0, &ctx1) != 0) return 0;
    if (ctx0 != 0) return 0;

    int slot = (int)ctx1;
    if (slot < 0 || slot >= TM_PMDIR_MAX_OPEN) return 0;
    if (g_slots[slot].badge == badge) g_slots[slot].badge = 0;
    return 0;
}

int tm_pmdir_stat(tm_stat_t *out)
{
    if (!out) return -EINVAL;
    unsigned char *p = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof *out; ++i) p[i] = 0;
    out->st_dev     = 1;
    out->st_ino     = 16;                    /* synthetic */
    out->st_mode    = TM_S_IFDIR | 0555;
    out->st_nlink   = 2;
    out->st_blksize = 4096;
    return 0;
}
