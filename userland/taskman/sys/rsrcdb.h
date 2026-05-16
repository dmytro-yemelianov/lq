/*
 * sys/rsrcdb.h — Resource Manager Database, taskman side.
 *
 * Per-class sorted lists of [start, end] ranges, each FREE or owned
 * by a process.  Backs the TM_REQ_RSRC_* wire ops.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_RSRCDB_H
#define QSOE_TASKMAN_RSRCDB_H

#include "../sel4_types.h"
#include <sys/rsrcdbmgr.h>

void tm_rsrc_init(void);

/* Boot-time seeding from the syscfg blob.  Picks up MEMORY tags,
 * registers them as RSRCDBMGR_MEMORY ranges, etc.                 */
void tm_rsrc_seed_from_syscfg(void);

/* Wire-op handlers — called from main.c's dispatcher.  Each takes
 * the caller's pid + entry count + payload pointer.  CREATE /
 * DESTROY take rsrc_alloc_t entries; ATTACH / DETACH take
 * rsrc_request_t entries (with the granted range echoed back).   */
int tm_rsrc_create  (pid_t caller, unsigned count);
int tm_rsrc_destroy (pid_t caller, unsigned count);
int tm_rsrc_attach  (pid_t caller, unsigned count);
int tm_rsrc_detach  (pid_t caller, unsigned count);
int tm_rsrc_query   (pid_t caller, unsigned listcnt, unsigned start,
                     uint32_t type, unsigned *out_written);

/* Process-exit hook: release every entry owned by this pid.       */
void tm_rsrc_release_pid(pid_t pid);

#endif /* QSOE_TASKMAN_RSRCDB_H */
