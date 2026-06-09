/*
 * path/sysfs.h -- LQ wiring for the synthetic read-only /sys tree.
 *
 * The /sys MODEL (entries, resolution, content, listing) lives in the
 * shared libtaskman core <tm_sysfs.h>.  This file is the LQ-side glue:
 * it gathers the four strings from taskman's syscfg blob + the version
 * header (tm_sysfs_populate), and serves opens/reads/readdirs/etc. via
 * the path/io.c handler_kind dispatch (PATHMGR_HANDLER_TASKMAN_SYSFS,
 * TM_SYSFS_CHID) -- the same in-taskman-resmgr pattern as cpiofs/pmdir.
 * NQ serves the same core through its own OCB framework instead.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_SYSFS_H
#define QSOE_TASKMAN_SYSFS_H

#include "../sel4_types.h"
#include <sys/qsoe.h>     /* tm_stat_t */

/* Gather board/cmdline/version/builddate from syscfg + the version
 * header and hand them to the shared core (tm_sysfs_init).  Call after
 * tm_syscfg_build(). */
void tm_sysfs_populate(void);

/* Per-fd handlers, called from path/io.c when a badge resolves to
 * (taskman, TM_SYSFS_CHID).  Each tracks per-open state (which entry,
 * read cursor) keyed by the connection badge. */
int tm_sysfs_open(const char *path, seL4_Word badge);
int tm_sysfs_read(seL4_Word badge, unsigned want, unsigned *out_got);
int tm_sysfs_readdir(seL4_Word badge, char *name_out,
                     unsigned *namelen_out, int *d_type_out);
int tm_sysfs_fstat(seL4_Word badge, tm_stat_t *out);
int tm_sysfs_lseek(seL4_Word badge, int whence, long offset, long *out_off);
int tm_sysfs_close(seL4_Word badge);

#endif /* QSOE_TASKMAN_SYSFS_H */
