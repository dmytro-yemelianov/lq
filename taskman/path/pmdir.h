/*
 * path/pmdir.h — synthetic directory handler backed by the pathmgr.
 *
 * Used for paths like /dev that have no on-disk reality but are
 * implicitly directories because the pathmgr's tree has child
 * entries below them.  ls(1) opens the path, reads its children via
 * readdir, sees the registered device names (null, zero, console,
 * tty, slog, ser1, pci, ...).
 *
 * State: a per-open slot mirroring cpiofs's dir-slot table, scoped
 * to TM_PMDIR_MAX_OPEN simultaneously-open synthetic directories.
 * Each slot remembers the path prefix (e.g. "/dev") and the next
 * child index to return.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_PATH_PMDIR_H
#define QSOE_TASKMAN_PATH_PMDIR_H

#include "../sel4_types.h"
#include <qsoe-system.h>     /* tm_stat_t */

/* Open a synthetic pathmgr directory at `path` (already known to
 * resolve to PATHMGR_HANDLER_TASKMAN_PMDIR).  Allocates a per-badge
 * dir slot; subsequent tm_pmdir_readdir() calls iterate the
 * children of `path` in pathmgr's tree. */
int tm_pmdir_open(const char *path, seL4_Word badge);

/* Return the next child's name + d_type into name_out/namelen_out/
 * d_type_out.  Returns 0 on success, -ENOENT past the end, -EBADF
 * if the badge has no allocated slot. */
int tm_pmdir_readdir(seL4_Word badge, char *name_out,
                     unsigned *namelen_out, int *d_type_out);

/* Free the slot owned by `badge`.  Called by io.c on close(). */
int tm_pmdir_close(seL4_Word badge);

/* Fill `out` with a synthetic-directory stat (S_IFDIR | 0555). */
int tm_pmdir_stat(tm_stat_t *out);

#endif
