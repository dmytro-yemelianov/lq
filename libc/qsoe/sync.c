/*
 * sync.c — QNX/QRV-style Sync* primitives (mutex / condvar / sem).
 *
 * Userspace fast path: __atomic_compare_exchange_n on the sync_t's
 * `owner` (mutex) / `count` (sem) field.  Uncontended operations
 * never enter taskman.
 *
 * Slow path: TM_REQ_SYNC_WAIT / TM_REQ_SYNC_WAKE — the two-mode
 * address-keyed primitive lives in taskman/sys/sync.c.  See
 * qsoe/wire.h for the mr0..mr2 layout.
 *
 * Mode choice per primitive:
 *   Mutex / Sem use WAIT mode 0 (credit-absorb): the unlock side
 *     calls WAKE absorb=1 even when it doesn't know whether anyone
 *     is parked, so any wake delivered before its matching wait
 *     is held as a single-shot credit instead of being lost.
 *   Cond uses WAIT mode 1 (gen-check): the user's last-observed
 *     cond->count is passed as expected_gen; taskman's tracked gen
 *     for the cond's address advances on every Signal/WAKE; if a
 *     Signal raced ahead, gen mismatches and WAIT returns
 *     immediately without parking.  Cond uses WAKE mode 1
 *     (discard) since POSIX permits losing a Signal that has no
 *     blocked waiter at the time.
 *
 * No priority inheritance in v0.8 — see [[project_sync_design]];
 * lands together with the seL4/MCS switch.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <qsoe/tls.h>

#include "sel4_types.h"
#include "qsoe_invoke.h"

/* ---- slow-path wrappers -------------------------------------------- */

static int sync_wait_credit(volatile void *addr)
{
    seL4_Word mr0 = (seL4_Word)addr;
    seL4_Word mr1 = 0;          /* mode 0: credit-absorb */
    seL4_Word mr2 = 0;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_SYNC_WAIT, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }
    return 0;
}

static int sync_wait_gen(volatile void *addr, long expected)
{
    seL4_Word mr0 = (seL4_Word)addr;
    seL4_Word mr1 = 1;          /* mode 1: gen-check */
    seL4_Word mr2 = (seL4_Word)expected;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_SYNC_WAIT, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }
    return 0;
}

/* WAKE; mode 0 deposits credit, mode 1 discards on no-waiter. */
static int sync_wake(volatile void *addr, int max_n, int mode)
{
    seL4_Word mr0 = (seL4_Word)addr;
    seL4_Word mr1 = (seL4_Word)max_n;
    seL4_Word mr2 = (seL4_Word)mode;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_SYNC_WAKE, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }
    return 0;
}

/* ---- public API ---------------------------------------------------- */

long SyncTypeCreate_r(unsigned type, sync_t *s,
                       const struct _sync_attr *attr)
{
    if (!s) return -EINVAL;
    /* For v0.8 the type just gates which field set the caller will
     * use; we don't actually keep per-sync_t state in taskman until
     * a wait fires.  Initialise both fields so a Lock after Create
     * sees a clean 0/0. */
    s->owner = 0;
    s->count = 0;
    if (type == QSOE_SYNC_SEM && attr && attr->count > 0) {
        s->count = attr->count;
    }
    (void)type;
    return 0;
}

int SyncTypeCreate(unsigned type, sync_t *s,
                   const struct _sync_attr *attr)
{
    long r = SyncTypeCreate_r(type, s, attr);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return 0;
}

int SyncDestroy(sync_t *s)
{
    /* Nothing kernel-side to release in v0.8.  The taskman per-(pid,
     * addr) wait-list entry is allocated on first WAIT and freed
     * when its wait list drains and credit count is zero; not
     * eagerly torn down here.  When a real cross-process Sync*
     * arrives (shm-mapped sync_t), this will free the registration. */
    if (!s) { qsoe_errno = EINVAL; return -1; }
    s->owner = 0;
    s->count = 0;
    return 0;
}

/* ---- Mutex --------------------------------------------------------- */

int SyncMutexLock(sync_t *s)
{
    if (!s) { qsoe_errno = EINVAL; return -1; }
    unsigned long my_tid = (unsigned long)qsoe_curthr()->tid;

    for (;;) {
        unsigned long val = __atomic_load_n(&s->owner, __ATOMIC_ACQUIRE);
        unsigned long tid_only = val & QSOE_SYNC_TID_MASK;

        if (tid_only == 0) {
            /* Free.  Try to take ownership while preserving any
             * pre-existing WAITERS bit (so a concurrent unlocker
             * that set it doesn't lose the wake credit). */
            unsigned long want = my_tid | (val & QSOE_SYNC_WAITERS);
            if (__atomic_compare_exchange_n(&s->owner, &val, want,
                                             0, __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED))
                return 0;
            continue;       /* CAS lost the race; retry */
        }

        if (tid_only == my_tid) {
            /* Recursive acquire by the current holder. */
            ++s->count;
            return 0;
        }

        /* Owned by someone else.  Make sure WAITERS is set so the
         * unlocker fires a WAKE for us. */
        if (!(val & QSOE_SYNC_WAITERS)) {
            unsigned long want = val | QSOE_SYNC_WAITERS;
            if (!__atomic_compare_exchange_n(&s->owner, &val, want,
                                              0, __ATOMIC_RELAXED,
                                              __ATOMIC_RELAXED))
                continue;   /* owner changed under us; retry whole loop */
        }

        /* Park.  Credit mode: if a WAKE already fired before us,
         * taskman has stored the credit and our wait will return
         * immediately. */
        if (sync_wait_credit(&s->owner) != 0) return -1;
        /* Loop and re-evaluate state. */
    }
}

int SyncMutexUnlock(sync_t *s)
{
    if (!s) { qsoe_errno = EINVAL; return -1; }
    unsigned long my_tid = (unsigned long)qsoe_curthr()->tid;
    unsigned long val = __atomic_load_n(&s->owner, __ATOMIC_RELAXED);

    if ((val & QSOE_SYNC_TID_MASK) != my_tid) {
        qsoe_errno = EPERM;
        return -1;
    }
    if (s->count > 0) {
        --s->count;
        return 0;
    }

    /* Release ownership (clear tid + WAITERS atomically) and wake
     * one if WAITERS was set.  Race-tolerant: an absorbing WAKE on
     * an empty wait queue parks a credit for the next locker. */
    unsigned long old = __atomic_exchange_n(&s->owner, 0,
                                             __ATOMIC_RELEASE);
    if (old & QSOE_SYNC_WAITERS) {
        return sync_wake(&s->owner, 1, 0 /* absorb */);
    }
    return 0;
}

/* ---- Condvar ------------------------------------------------------- */

/* POSIX requires the caller to hold the mutex; we honor that contract
 * — Wait reads cond->count under the mutex, unlocks, then issues a
 * gen-checked wait.  Spurious wakeups (gen mismatch) cause an early
 * return so the caller's while-loop discipline handles them. */
int SyncCondvarWait(sync_t *cond, sync_t *mutex)
{
    if (!cond || !mutex) { qsoe_errno = EINVAL; return -1; }
    long seen = __atomic_load_n(&cond->count, __ATOMIC_ACQUIRE);

    int rc = SyncMutexUnlock(mutex);
    if (rc) return -1;

    /* If a Signal raced ahead between unlock and here, taskman's
     * tracked gen for this addr is now > seen, sync_wait_gen
     * returns without parking. */
    rc = sync_wait_gen(&cond->count, seen);

    /* Re-acquire the mutex even if wait returned -1 — POSIX says
     * cond_wait always returns with the mutex held. */
    SyncMutexLock(mutex);
    return rc;
}

int SyncCondvarSignal(sync_t *cond, int wake_all)
{
    if (!cond) { qsoe_errno = EINVAL; return -1; }
    /* Increment the gen the waiters compare against, then wake.
     * Discard mode: signals with no parked waiter are dropped
     * (POSIX-compliant). */
    __atomic_fetch_add(&cond->count, 1, __ATOMIC_RELEASE);
    return sync_wake(&cond->count, wake_all ? 0 : 1, 1 /* discard */);
}

/* ---- Semaphore ----------------------------------------------------- */

int SyncSemPost(sync_t *s)
{
    if (!s) { qsoe_errno = EINVAL; return -1; }
    /* fetch_add returns the previous value; if it was negative or
     * zero AND there are blocked waiters, we owe a wake.  Credit
     * mode absorbs the wake if our timing beats the waiter's WAIT
     * registration. */
    long old = __atomic_fetch_add(&s->count, 1, __ATOMIC_RELEASE);
    if (old < 1) {
        return sync_wake(&s->count, 1, 0 /* absorb */);
    }
    return 0;
}

/* Raw form: returns 0 on success or a negated errno, the QNX/QSOE `_r`
 * convention (the shared suite checks `SyncSemWait_r(...) == -EAGAIN`
 * for a non-blocking empty semaphore).  SyncSemWait wraps it into the
 * errno/-1 shape.  Mirrors nq/libc/api/sync.c's _r split. */
long SyncSemWait_r(sync_t *s, int try_only)
{
    if (!s) return -EINVAL;
    for (;;) {
        long c = __atomic_load_n(&s->count, __ATOMIC_ACQUIRE);
        if (c > 0) {
            if (__atomic_compare_exchange_n(&s->count, &c, c - 1,
                                             0, __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED))
                return 0;
            continue;       /* race; retry */
        }
        if (try_only) return -EAGAIN;
        if (sync_wait_credit(&s->count) != 0) {
            int e = qsoe_errno;
            return e ? -(long)e : -EINTR;
        }
        /* Loop; re-attempt to claim. */
    }
}

int SyncSemWait(sync_t *s, int try_only)
{
    long r = SyncSemWait_r(s, try_only);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return (int)r;
}
