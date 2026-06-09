/*
 * path/pathmgr.h -- LQ-side shim over <libtaskman/pathmgr.h>.
 *
 * The portable namespace-registry surface (tm_pathmgr_register / resolve /
 * repath / symlink / child_at + tm_pathmgr_obj_t + PATHMGR_HANDLER_*) now
 * lives in libtaskman.  This shim re-exports it for existing LQ taskman
 * call sites and adds the LQ-only channel-id constants used by main.c to
 * register the in-taskman handlers (console, cpiofs, pmdir, /dev/null,
 * /dev/zero) on the primary endpoint.
 *
 * Phase 3 migration (2026-06-01): once every LQ source has been updated
 * to include <tm_pathmgr.h> directly, this file shrinks to just
 * the channel-id constants (likely renamed to path/tm_chids.h).  Until
 * then the shim keeps the source-level disruption minimal.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_PATHMGR_H
#define QSOE_TASKMAN_PATHMGR_H

#include <tm_pathmgr.h>

/* Internal taskman channel ids beyond the primary (1).  All share the
 * primary endpoint; (TASKMAN_PID, *_CHID) is registered in the channel
 * table so ConnectAttach can mint badged Send caps onto them, and the
 * dispatch loop routes IO_WRITE / IO_READ by checking which channel
 * each badge points at.  LQ-specific -- NQ has its own routing scheme. */
#define TM_CONSOLE_CHID  2
#define TM_CPIOFS_CHID   3
#define TM_DEVNULL_CHID  4
#define TM_DEVZERO_CHID  5
#define TM_PMDIR_CHID    6   /* synthetic pathmgr dirs (/dev) */
#define TM_SYSFS_CHID    7   /* synthetic read-only /sys */
#define TM_PROCFS_CHID   8   /* synthetic read-only /proc */

#endif /* QSOE_TASKMAN_PATHMGR_H */
