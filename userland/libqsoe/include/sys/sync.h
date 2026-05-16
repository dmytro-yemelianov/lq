/*
 * sys/sync.h — QNX/QRV-shaped synchronization primitives.
 *
 * Public surface for SyncTypeCreate / SyncMutex* / SyncCondvar* /
 * SyncSem* — the QNX-style sync API that drivers and library code
 * use for in-process and (later) cross-process synchronization.
 *
 * Implementation strategy (v0.8):
 *   - Userspace fast path: atomic CAS on the sync_t's `owner` (mutex)
 *     or `count` (sem).  No syscall when uncontended.
 *   - Slow path on contention: TM_REQ_SYNC_WAIT / TM_REQ_SYNC_WAKE
 *     (see qsoe/wire.h) into taskman's address-keyed wait list.
 *   - Wake primitive: seL4 reply caps parked via SaveCaller, matching
 *     the pattern used by nanosleep / itimer.
 *
 * Priority inheritance is intentionally absent in v0.8 — see the
 * [[project_sync_design]] memo; deferred to v1.0 when the seL4/MCS
 * switch makes sched-context donation native.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_SYS_SYNC_H
#define QSOE_SYS_SYNC_H

#include <qsoe-system.h>

/* The public sync object — two machine words.  Matches QRV's layout
 * (count + owner) so source ports across with no struct surgery.
 *
 * Field semantics depend on the type:
 *   MUTEX_FREE: owner = tid_or_zero | QRV_SYNC_WAITERS bit
 *               count = recursive nest level
 *   SEM:        owner = unused
 *               count = available count (negative when waiters queued)
 *   COND:       owner = unused
 *               count = generation counter (incremented on each Signal)
 */
typedef struct _sync {
    long          count;
    unsigned long owner;
} sync_t;

/* Bit allocation in `owner` (mutex only).  The low 31 bits hold the
 * owning tid; bit 31 says "at least one waiter has been parked in
 * taskman and is awaiting wake".  Both unlocker and locker
 * test-and-set this bit atomically. */
#define QRV_SYNC_WAITERS         0x80000000UL
#define QRV_SYNC_TID_MASK        0x7fffffffUL

/* Sync types (first arg to SyncTypeCreate).  Numeric values pinned
 * so source-compat with QRV stays. */
#define QRV_SYNC_MUTEX_FREE      0x00
#define QRV_SYNC_COND            0x01
#define QRV_SYNC_SEM             0x02
#define QRV_SYNC_MUTEX_NONRECURSIVE  0x03  /* same wire as FREE, attr-only */

/* Attribute flags (third arg to SyncTypeCreate via _sync_attr). */
#define QRV_SYNC_INITIALIZER     { 0, 0 }
#define QRV_SYNC_NONRECURSIVE    0x01
#define QRV_SYNC_PRIOCEILING     0x02   /* attr accepted, ignored in v0.8 */
#define QRV_SYNC_PRIVATE         0x80   /* in-process only (default) */

struct _sync_attr {
    int    protocol;        /* QRV_SYNC_PRIO_INHERIT etc. — ignored v0.8 */
    int    flags;           /* QRV_SYNC_* attribute bits above           */
    int    prioceiling;     /* ignored v0.8                              */
    int    clockid;         /* CLOCK_REALTIME / CLOCK_MONOTONIC — sem    */
    int    count;           /* initial sem count                         */
    int    _reserved[3];
};

/* Cap on simultaneously-blocked waiters across the system.  Each
 * wait costs one slot in taskman's per-sync table; sized for the
 * v0.8 expected load (handful of mutexes per driver).  Bump together
 * with tm_sync_init() if a real workload hits this. */
#define QRV_SYNC_MAX_WAITERS     64

/* --- API surface ----------------------------------------------------- */

int SyncTypeCreate     (unsigned type, sync_t *sync,
                        const struct _sync_attr *attr);
int SyncDestroy        (sync_t *sync);

int SyncMutexLock      (sync_t *sync);
int SyncMutexUnlock    (sync_t *sync);

int SyncCondvarWait    (sync_t *cond, sync_t *mutex);
int SyncCondvarSignal  (sync_t *cond, int wake_all);

int SyncSemPost        (sync_t *sem);
int SyncSemWait        (sync_t *sem, int try_only);

#endif /* QSOE_SYS_SYNC_H */
