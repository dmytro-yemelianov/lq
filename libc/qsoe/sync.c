/*
 * sync.c — QNX/QRV-style Sync* primitives (mutex / condvar / sem).
 *
 * Userspace fast path: __atomic_compare_exchange_n on the sync_t's
 * `owner` (mutex) / `count` (sem) field.  Uncontended operations
 * never enter the kernel and never message taskman.
 *
 * Slow path (PoC): an in-process, address-keyed wait/wake table that
 * blocks and wakes threads through *kernel Notifications* — never
 * through taskman.  This is the same data model as taskman/sys/sync.c
 * (credit-absorb + gen-check, a small linear table), moved into the
 * process and re-plumbed onto seL4 primitives:
 *
 *   taskman model            ->  in-process model (here)
 *   ----------------------       --------------------------------------
 *   keyed by (pid, addr)         keyed by addr (one address space)
 *   park = stash reply object    park = push this thread's Notification
 *   wake = reply on the object   wake = qsoe_sys_signal(Notification)
 *   table is single-threaded     table guarded by a Notification-as-mutex
 *   in the taskman dispatcher
 *
 * Why this matters: routing the slow path through taskman deadlocks
 * when taskman is itself a blocked client of a server whose own
 * threads need to wake each other (e.g. devb-nvme's IST signalling its
 * I/O worker while taskman waits on a spawn-image read from fs-qrv).
 * Going straight through the kernel makes the wake independent of
 * taskman's state — exactly like NQ, where Skimmer mediates Sync.
 *
 * Per-thread block Notification: worker threads already own one
 * (taskman mints `join_ntfn` at THREAD_ALLOC); the main thread gets
 * one minted at qsoe_sync_init().  seL4_Wait is sticky (a Signal
 * delivered before the matching Wait sets a pending bit the Wait then
 * consumes), so it serves as a futex-style park with no lost-wakeup in
 * the unlock→park window.  Reuse of join_ntfn is safe: a thread parked
 * in a Sync wait is never simultaneously exiting to signal a joiner.
 *
 * Table lock: a Notification used as a *blocking* binary-semaphore
 * mutex, NOT a CAS spinlock.  A spinlock livelocks here — the prio-21
 * IST can preempt a prio-10 lock holder and busy-wait forever, since
 * the holder never regains the CPU under strict priority.  A blocking
 * mutex makes the IST yield instead; the holder runs, releases, and the
 * IST proceeds (bounded priority inversion).
 *
 * Mode choice per primitive:
 *   Mutex / Sem use WAIT credit-absorb: the unlock side WAKEs absorb
 *     even without knowing whether anyone is parked, so a wake
 *     delivered before its matching wait is held as a single-shot
 *     credit instead of being lost.
 *   Cond uses WAIT gen-check: the user's last-observed cond->count is
 *     passed as expected; the tracked gen for the address advances on
 *     every Signal/WAKE; if a Signal raced ahead, gen mismatches and
 *     WAIT returns immediately without parking.  Cond uses WAKE discard
 *     since POSIX permits losing a Signal that has no blocked waiter.
 *
 * No priority inheritance yet — see [[project_sync_design]].
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include <qsoe/tls.h>

#include "state.h"          /* qsoe_state_alloc_empty_slot, qsoe_sync_init */
#include "sel4_types.h"
#include "qsoe_invoke.h"

/* ---- in-process wait/wake table ------------------------------------ */

/* Mirrors taskman/sys/sync.h, the model this replaces.  CREDIT =
 * mutex/sem (absorb a pending wake); GEN = cond (park iff gen matches). */
enum { SYNC_WAIT_CREDIT = 0, SYNC_WAIT_GEN = 1 };
/* ABSORB = deposit a credit if no waiter; DISCARD = drop it. */
enum { SYNC_WAKE_ABSORB = 0, SYNC_WAKE_DISCARD = 1 };

#define SYNC_MAX_ENTRIES   16   /* distinct addresses in flight at once  */
#define SYNC_MAX_WAITERS    4   /* parked threads per address            */

typedef struct {
    unsigned long addr;                     /* sync_t field address (key) */
    int           in_use;
    int           credit;                   /* absorbing-WAKE deposits    */
    long          gen;                       /* advances on every WAKE     */
    int           nwaiters;
    seL4_CPtr     waiters[SYNC_MAX_WAITERS]; /* parked threads' Notifs     */
} sync_entry_t;

static sync_entry_t g_sync[SYNC_MAX_ENTRIES];

/* Notification used as a blocking mutex over g_sync[].  0 until
 * qsoe_sync_init() mints + arms it; see the file banner for why this is
 * a blocking primitive and not a spinlock. */
static seL4_CPtr g_table_lock;

/* ---- kernel-object minting (taskman-free) -------------------------- */

/* Retype one seL4 Notification out of our own untyped budget into a
 * fresh CSpace slot.  Invokes the Untyped cap directly on the kernel —
 * no taskman round-trip — so it is safe even while taskman is blocked.
 * Returns the slot (a usable Notification cap) or 0 on exhaustion. */
static seL4_CPtr mint_notification(void)
{
    unsigned long slot = qsoe_state_alloc_empty_slot();
    if (!slot) return 0;
    seL4_Word err = qsoe_untyped_retype(QSOE_CAP_OWN_UNTYPED,
                                        seL4_NotificationObject, 0,
                                        QSOE_CAP_CNODE_SELF, 0, 0,
                                        slot, 1);
    if (err) { qsoe_state_free_empty_slot(slot); return 0; }
    return (seL4_CPtr)slot;
}

/* This thread's block Notification.  Workers carry join_ntfn from
 * taskman; the main thread (and any thread that lacks one) mints its
 * own lazily — only the owning thread writes its join_ntfn, so no race. */
static seL4_CPtr block_ntfn_self(void)
{
    qsoe_tcb_t *t = qsoe_curthr();
    if (t->join_ntfn) return (seL4_CPtr)t->join_ntfn;
    seL4_CPtr n = mint_notification();
    if (n) t->join_ntfn = (unsigned long)n;
    return n;
}

/* One-time per-process setup: mint + arm the table-lock Notification and
 * give the main thread its block Notification.  Called from
 * qsoe_libc_init() while still single-threaded, so the guard never
 * races; the slow path below also calls it defensively. */
void qsoe_sync_init(void)
{
    if (g_table_lock) return;
    seL4_CPtr lock = mint_notification();
    if (!lock) return;          /* slow path announces on first use */
    qsoe_sys_signal(lock);      /* one token: the mutex starts available */
    g_table_lock = lock;
    (void)block_ntfn_self();    /* main thread's park Notification */
}

static void table_lock(void)
{
    if (!g_table_lock) qsoe_sync_init();
    if (g_table_lock) qsoe_sys_wait(g_table_lock);
}

static void table_unlock(void)
{
    if (g_table_lock) qsoe_sys_signal(g_table_lock);
}

/* ---- table helpers (addr-keyed; mirror taskman/sys/sync.c) --------- */

static sync_entry_t *find_entry(unsigned long addr)
{
    for (int i = 0; i < SYNC_MAX_ENTRIES; ++i)
        if (g_sync[i].in_use && g_sync[i].addr == addr) return &g_sync[i];
    return 0;
}

static sync_entry_t *alloc_entry(unsigned long addr)
{
    for (int i = 0; i < SYNC_MAX_ENTRIES; ++i) {
        if (g_sync[i].in_use) continue;
        sync_entry_t *e = &g_sync[i];
        e->addr = addr; e->in_use = 1;
        e->credit = 0;  e->gen = 0; e->nwaiters = 0;
        return e;
    }
    return 0;
}

/* Drop an entry with no waiters and no pending credit. */
static void retire_if_idle(sync_entry_t *e)
{
    if (e && e->in_use && e->nwaiters == 0 && e->credit == 0)
        e->in_use = 0;
}

static int push_waiter(sync_entry_t *e, seL4_CPtr n)
{
    if (e->nwaiters >= SYNC_MAX_WAITERS) return -1;
    e->waiters[e->nwaiters++] = n;
    return 0;
}

static seL4_CPtr pop_waiter(sync_entry_t *e)
{
    if (e->nwaiters == 0) return 0;
    seL4_CPtr n = e->waiters[0];
    for (int i = 1; i < e->nwaiters; ++i) e->waiters[i - 1] = e->waiters[i];
    --e->nwaiters;
    return n;
}

/* Remove a specific Notification from the FIFO (post-wait cleanup for a
 * spurious return; a no-op if the waker already popped us). */
static void remove_waiter(sync_entry_t *e, seL4_CPtr n)
{
    for (int i = 0; i < e->nwaiters; ++i) {
        if (e->waiters[i] != n) continue;
        for (int j = i + 1; j < e->nwaiters; ++j) e->waiters[j - 1] = e->waiters[j];
        --e->nwaiters;
        return;
    }
}

/* ---- slow-path wait/wake (kernel Notifications, no taskman) -------- */

static int sync_wait_impl(volatile void *vaddr, unsigned mode, long expected)
{
    unsigned long addr = (unsigned long)vaddr;
    if (!addr) { qsoe_errno = EINVAL; return -1; }

    /* Mint our park Notification BEFORE taking the table lock, so the
     * (possible) retype syscall doesn't lengthen the critical section. */
    seL4_CPtr myn = block_ntfn_self();
    if (!myn) { qsoe_errno = ENOMEM; return -1; }

    table_lock();
    sync_entry_t *e = find_entry(addr);

    if (mode == SYNC_WAIT_GEN) {
        /* Cond: a Signal since the caller sampled `expected` advanced the
         * gen — don't park.  No entry + non-zero expected means signals
         * ran before any wait; treat as "wake already arrived". */
        if (e && e->gen != expected) { retire_if_idle(e); table_unlock(); return 0; }
        if (!e && expected != 0)     { table_unlock(); return 0; }
    } else {
        /* Mutex/sem: consume a deposited credit instead of parking. */
        if (e && e->credit > 0) { --e->credit; retire_if_idle(e); table_unlock(); return 0; }
    }

    if (!e) {
        e = alloc_entry(addr);
        if (!e) { table_unlock(); qsoe_errno = ENOMEM; return -1; }
    }
    if (push_waiter(e, myn) != 0) {
        retire_if_idle(e); table_unlock(); qsoe_errno = ENOMEM; return -1;
    }
    table_unlock();

    /* Block outside the lock.  A wake racing between unlock and here sets
     * myn's pending bit; the Wait then returns at once (Notifications are
     * sticky) — no lost wakeup. */
    qsoe_sys_wait(myn);

    table_lock();
    e = find_entry(addr);           /* re-find: may have been retired/reused */
    if (e) { remove_waiter(e, myn); retire_if_idle(e); }
    table_unlock();
    return 0;
}

static int sync_wake_impl(volatile void *vaddr, int max_n, unsigned mode)
{
    unsigned long addr = (unsigned long)vaddr;
    if (!addr) { qsoe_errno = EINVAL; return -1; }

    table_lock();
    sync_entry_t *e = find_entry(addr);
    if (e) e->gen += 1;                       /* cond waiters compare this */

    if (max_n == 0) max_n = SYNC_MAX_WAITERS;  /* 0 = wake all */
    int woken = 0;
    if (e) {
        while (woken < max_n) {
            seL4_CPtr n = pop_waiter(e);
            if (!n) break;
            qsoe_sys_signal(n);
            ++woken;
        }
    }

    if (woken < max_n && mode == SYNC_WAKE_ABSORB) {
        /* Nobody to wake; deposit one single-shot credit so the next
         * credit-mode WAIT consumes it instead of parking. */
        if (!e) {
            e = alloc_entry(addr);
            if (!e) { table_unlock(); qsoe_errno = ENOMEM; return -1; }
            e->gen = 1;                        /* this WAKE counts too */
        }
        if (e->credit < 1) e->credit = 1;
    }

    retire_if_idle(e);
    table_unlock();
    return 0;
}

/* ---- slow-path wrappers (same shape the public API below calls) ---- */

static int sync_wait_credit(volatile void *addr)
{
    return sync_wait_impl(addr, SYNC_WAIT_CREDIT, 0);
}

static int sync_wait_gen(volatile void *addr, long expected)
{
    return sync_wait_impl(addr, SYNC_WAIT_GEN, expected);
}

/* WAKE; mode 0 deposits credit, mode 1 discards on no-waiter. */
static int sync_wake(volatile void *addr, int max_n, int mode)
{
    return sync_wake_impl(addr, max_n, (unsigned)mode);
}

/* ---- public API ---------------------------------------------------- */

/* owner sentinel for a destroyed sync object (see <sys/_synctypes.h>:
 * "-2  destroyed mutex").  SyncDestroy plants it; SyncMutexLock rejects
 * it with EINVAL, matching QNX's behavior on a destroyed mutex. */
#define QSOE_SYNC_DESTROYED  ((unsigned long)-2)

long SyncTypeCreate_r(unsigned type, sync_t *s,
                       const struct _sync_attr *attr)
{
    if (!s) return -EINVAL;

    /* Validate the requested type (QNX: EINVAL on an unknown type). */
    switch (type) {
    case QSOE_SYNC_MUTEX_FREE:
    case QSOE_SYNC_COND:
    case QSOE_SYNC_SEM:
    case QSOE_SYNC_MUTEX_NONRECURSIVE:
        break;
    default:
        return -EINVAL;
    }

    if (attr) {
        /* Mutexes: the protocol must be a known PTHREAD_PRIO_* analogue
         * (QNX: EINVAL otherwise). */
        if (type == QSOE_SYNC_MUTEX_FREE ||
            type == QSOE_SYNC_MUTEX_NONRECURSIVE) {
            if (attr->protocol != QSOE_PRIO_NONE &&
                attr->protocol != QSOE_PRIO_INHERIT &&
                attr->protocol != QSOE_PRIO_PROTECT)
                return -EINVAL;
        }
        /* Condvars carry a clockid (QSOE extension to QNX's attr); reject
         * anything outside the defined CLOCK_* range. */
        if (type == QSOE_SYNC_COND) {
            if (attr->clockid < CLOCK_REALTIME ||
                attr->clockid > CLOCK_THREAD_CPUTIME_ID)
                return -EINVAL;
        }
    }

    /* For v0.8 the type just gates which field set the caller will
     * use; we don't actually keep per-sync_t state in taskman until
     * a wait fires.  Initialise both fields so a Lock after Create
     * sees a clean 0/0. */
    s->owner = 0;
    s->count = 0;
    if (type == QSOE_SYNC_SEM && attr && attr->count > 0) {
        s->count = attr->count;
    }
    return 0;
}

int SyncTypeCreate(unsigned type, sync_t *s,
                   const struct _sync_attr *attr)
{
    long r = SyncTypeCreate_r(type, s, attr);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return 0;
}

/* Reentrant core: 0 on success, NEGATIVE errno on failure -- never touch
 * qsoe_errno.  pthread_mutex_destroy / pthread_cond_destroy (shared libc)
 * call this; SyncDestroy() below is the errno-setting wrapper.  Matches
 * NQ's SyncDestroy_r so one pthread layer serves both kernels. */
long SyncDestroy_r(sync_t *s)
{
    /* Nothing kernel-side to release in v0.8.  The taskman per-(pid,
     * addr) wait-list entry is allocated on first WAIT and freed
     * when its wait list drains and credit count is zero; not
     * eagerly torn down here.  When a real cross-process Sync*
     * arrives (shm-mapped sync_t), this will free the registration. */
    if (!s) return -EINVAL;
    /* Plant the destroyed sentinel so a later operation on this object
     * fails with EINVAL rather than silently succeeding (a destroyed
     * mutex looks "free" if we just zero it).  A fresh SyncTypeCreate at
     * the same address clears it. */
    s->count = 0;
    s->owner = QSOE_SYNC_DESTROYED;
    return 0;
}

int SyncDestroy(sync_t *s)
{
    long r = SyncDestroy_r(s);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return 0;
}

/* ---- Mutex --------------------------------------------------------- */

int SyncMutexLock(sync_t *s)
{
    if (!s) { qsoe_errno = EINVAL; return -1; }
    if (__atomic_load_n(&s->owner, __ATOMIC_ACQUIRE) == QSOE_SYNC_DESTROYED) {
        qsoe_errno = EINVAL;            /* locking a destroyed mutex */
        return -1;
    }
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
/* The _r forms carry the real logic and follow the QNX/QSOE convention:
 * return 0 or a negated errno, never touch errno.  The shared libc
 * pthread_cond_* layer calls these; the int SyncCondvar* below are the
 * errno/-1 adapters.  (Mirrors SyncSemWait_r / SyncSemWait.) */
long SyncCondvarWait_r(sync_t *cond, sync_t *mutex)
{
    if (!cond || !mutex) return -EINVAL;
    long seen = __atomic_load_n(&cond->count, __ATOMIC_ACQUIRE);

    if (SyncMutexUnlock(mutex) != 0) {
        int e = qsoe_errno; return e ? -(long)e : -EINVAL;
    }

    /* If a Signal raced ahead between unlock and here, taskman's tracked
     * gen for this addr is now > seen, so sync_wait_gen returns without
     * parking.  Spurious returns are fine — the caller re-tests its
     * predicate under the re-acquired mutex. */
    int wr = sync_wait_gen(&cond->count, seen);

    /* POSIX: cond_wait always returns with the mutex held — re-acquire
     * even on a wait error. */
    SyncMutexLock(mutex);

    if (wr < 0) { int e = qsoe_errno; return e ? -(long)e : -EINTR; }
    return 0;
}

int SyncCondvarWait(sync_t *cond, sync_t *mutex)
{
    long r = SyncCondvarWait_r(cond, mutex);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return 0;
}

long SyncCondvarSignal_r(sync_t *cond, int wake_all)
{
    if (!cond) return -EINVAL;
    /* Increment the gen the waiters compare against, then wake.  Discard
     * mode: signals with no parked waiter are dropped (POSIX-compliant). */
    __atomic_fetch_add(&cond->count, 1, __ATOMIC_RELEASE);
    if (sync_wake(&cond->count, wake_all ? 0 : 1, 1 /* discard */) < 0) {
        int e = qsoe_errno; return e ? -(long)e : -EINVAL;
    }
    return 0;
}

int SyncCondvarSignal(sync_t *cond, int wake_all)
{
    long r = SyncCondvarSignal_r(cond, wake_all);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return 0;
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
