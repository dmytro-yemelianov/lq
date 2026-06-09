/*
 * path/procfs.h -- LQ wiring for the synthetic read-only /proc tree.
 *
 * The /proc MODEL (path resolution, the `info` text, the readdir walk)
 * lives in the shared libtaskman core <tm_procfs.h>.  This file is the
 * LQ-side glue: tm_procfs_populate() registers LQ's process-table
 * accessors with the core, and the per-fd handlers below serve
 * opens/reads/readdirs/etc. through path/io.c's handler_kind dispatch
 * (PATHMGR_HANDLER_TASKMAN_PROCFS, TM_PROCFS_CHID) -- the same
 * in-taskman-resmgr pattern as cpiofs/pmdir/sysfs.  NQ serves the same
 * core through its own OCB framework instead.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_PROCFS_H
#define QSOE_TASKMAN_PROCFS_H

#include "../sel4_types.h"
#include <sys/qsoe.h>     /* tm_stat_t */

/* Register LQ's process-table accessors with the shared core.  Call
 * once at boot (after the process table exists). */
void tm_procfs_populate(void);

/* Per-fd handlers, called from path/io.c when a badge resolves to
 * (taskman, TM_PROCFS_CHID).  Per-open state (which node, read/readdir
 * cursor) lives in the connection ctx keyed by badge. */
int tm_procfs_open(const char *path, seL4_Word badge);
int tm_procfs_read(seL4_Word badge, unsigned want, unsigned *out_got);
int tm_procfs_readdir(seL4_Word badge, char *name_out,
                      unsigned *namelen_out, int *d_type_out);
int tm_procfs_fstat(seL4_Word badge, tm_stat_t *out);
int tm_procfs_lseek(seL4_Word badge, int whence, long offset, long *out_off);
int tm_procfs_close(seL4_Word badge);

#endif /* QSOE_TASKMAN_PROCFS_H */
