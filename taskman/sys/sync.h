/*
 * sys/sync.h — taskman's Sync* slow-path.
 *
 * Backs TM_REQ_SYNC_WAIT / TM_REQ_SYNC_WAKE.  The QNX-shaped public
 * API (SyncMutexLock / SyncCondvarWait / SyncSemPost / ...) lives in
 * libqsoe and stays in userspace for the uncontended fast paths; only
 * contention reaches here.
 *
 * Storage: a small fixed table of (pid, vaddr) entries.  Each entry
 * carries an intrusive list of parked waiters (saved reply caps),
 * a wake-credit count (for mutex/sem absorbing WAKE-before-WAIT),
 * and a monotonic wake-generation (for condvar gen-check).  Entries
 * are allocated lazily on first WAIT or first absorbing WAKE.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_SYS_SYNC_H
#define QSOE_TASKMAN_SYS_SYNC_H

#include "../sel4_types.h"
#include <sys/qsoe.h>

/* WAIT mode constants — must match libqsoe's sync.c. */
#define TM_SYNC_WAIT_CREDIT   0   /* mutex/sem: absorb pending wakes */
#define TM_SYNC_WAIT_GEN      1   /* cond: park iff gen == expected */

/* WAKE mode constants. */
#define TM_SYNC_WAKE_ABSORB   0   /* deposit credit if no waiter */
#define TM_SYNC_WAKE_DISCARD  1   /* drop if no waiter */

/* TM_REQ_SYNC_WAIT — stashes the reply object (deferred reply) on park.
 * Returns 0 on immediate return (gen mismatched / credit consumed),
 * 1 when parked (caller sets out_no_reply), or a negative errno on
 * failure. */
int tm_sync_wait(pid_t caller, unsigned long addr,
                 unsigned mode, long expected, int *out_parked);

/* TM_REQ_SYNC_WAKE — wakes up to max_n parked waiters; absorb mode
 * deposits one credit if no waiter is present.  Always advances the
 * entry's wake-gen counter. */
int tm_sync_wake(pid_t caller, unsigned long addr, int max_n,
                 unsigned mode);

#endif /* QSOE_TASKMAN_SYS_SYNC_H */
