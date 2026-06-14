/*
 * sys/sync.c — taskman's address-keyed wait/wake for Sync*.
 *
 * One fixed-size table of entries, each keyed by (pid, vaddr).  An
 * entry is allocated lazily on the first WAIT or absorbing WAKE that
 * misses, and freed when its wait list empties AND its credit goes
 * to zero.  No hashing — a linear scan of a 64-entry table is fine for
 * the contention rates we see.  Bump TM_SYNC_MAX_ENTRIES (and the
 * matching QSOE_SYNC_MAX_WAITERS for waiters per entry) when profiling
 * says so.
 *
 * Parked replies use taskman's deferred-reply pattern: each waiter's
 * reply object is stashed in a CSpace slot, which is returned to the
 * pool when WAKE replies on it.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sync.h"
#include "../proc/proc.h"
#include "../qsoe_invoke.h"
#include <tm_log.h>

#define TM_SYNC_MAX_ENTRIES   16    /* (pid, addr) tuples in flight  */
#define TM_SYNC_MAX_WAITERS    4    /* per-entry parked thread cap   */

typedef struct {
    pid_t         pid;
    unsigned long addr;
    int           in_use;
    int           credit;                  /* absorbing-WAKE deposits */
    long          gen;                     /* advances on every WAKE  */
    int           nwaiters;
    seL4_CPtr     wait_slots[TM_SYNC_MAX_WAITERS];
} tm_sync_entry_t;

static tm_sync_entry_t g_sync[TM_SYNC_MAX_ENTRIES];

/* ---- table helpers ------------------------------------------------- */

static tm_sync_entry_t *find_entry(pid_t pid, unsigned long addr)
{
    for (int i = 0; i < TM_SYNC_MAX_ENTRIES; ++i) {
        if (!g_sync[i].in_use) continue;
        if (g_sync[i].pid == pid && g_sync[i].addr == addr) return &g_sync[i];
    }
    return 0;
}

static tm_sync_entry_t *alloc_entry(pid_t pid, unsigned long addr)
{
    for (int i = 0; i < TM_SYNC_MAX_ENTRIES; ++i) {
        if (g_sync[i].in_use) continue;
        tm_sync_entry_t *e = &g_sync[i];
        e->pid       = pid;
        e->addr      = addr;
        e->in_use    = 1;
        e->credit    = 0;
        e->gen       = 0;
        e->nwaiters  = 0;
        return e;
    }
    return 0;
}

/* Drop an entry if it has neither pending waiters nor pending credit
 * and no live work — keeps the table from filling with stale slots. */
static void retire_if_idle(tm_sync_entry_t *e)
{
    if (e && e->in_use && e->nwaiters == 0 && e->credit == 0) {
        e->in_use = 0;
    }
}

/* Push a saved reply cap into the per-entry FIFO.  Returns 0 on
 * success, -1 if full. */
static int push_waiter(tm_sync_entry_t *e, seL4_CPtr slot)
{
    if (e->nwaiters >= TM_SYNC_MAX_WAITERS) return -1;
    e->wait_slots[e->nwaiters++] = slot;
    return 0;
}

/* Pop the head waiter from the FIFO; returns 0 if empty. */
static seL4_CPtr pop_waiter(tm_sync_entry_t *e)
{
    if (e->nwaiters == 0) return 0;
    seL4_CPtr slot = e->wait_slots[0];
    for (int i = 1; i < e->nwaiters; ++i) {
        e->wait_slots[i - 1] = e->wait_slots[i];
    }
    --e->nwaiters;
    return slot;
}

/* ---- TM_REQ_SYNC_WAIT handler -------------------------------------- */

int tm_sync_wait(pid_t caller, unsigned long addr,
                 unsigned mode, long expected, int *out_parked)
{
    *out_parked = 0;
    if (addr == 0) return -EINVAL;

    tm_sync_entry_t *e = find_entry(caller, addr);

    if (mode == TM_SYNC_WAIT_GEN) {
        /* Cond-style.  If gen advanced past `expected`, a signal
         * already fired since the caller sampled it — don't park. */
        if (e && e->gen != expected) {
            retire_if_idle(e);
            return 0;
        }
        /* No entry (gen still 0, treat as match if expected==0) or
         * gen still matches.  Fall through to park. */
        if (!e && expected != 0) {
            /* Caller observed a non-zero cond->count but taskman
             * has no record of any WAKE on this address.  This can
             * happen if signals fired before any wait ever ran,
             * leaving the user counter ahead; treat as "wake
             * already arrived", return without parking. */
            return 0;
        }
    } else if (mode == TM_SYNC_WAIT_CREDIT) {
        /* Mutex/sem style.  If a WAKE deposited credit before our
         * arrival, consume one and return. */
        if (e && e->credit > 0) {
            --e->credit;
            retire_if_idle(e);
            return 0;
        }
    } else {
        return -EINVAL;
    }

    /* Need to park.  Allocate the entry if it doesn't exist yet. */
    if (!e) {
        e = alloc_entry(caller, addr);
        if (!e) return -ENOMEM;
    }

    /* Park the caller's reply object and add it to the wait list.
     * Check capacity FIRST so tm_reply_park() (which moves + replenishes
     * the dispatcher's reply object) is the last fallible step — undoing
     * a park after a later failure would strand the blocked caller. */
    if (e->nwaiters >= TM_SYNC_MAX_WAITERS) {
        retire_if_idle(e);
        return -ENOMEM;
    }
    seL4_CPtr slot = tm_reply_park();
    if (slot == 0) {
        retire_if_idle(e);
        return -ENOMEM;
    }
    push_waiter(e, slot);   /* room guaranteed by the check above */

    *out_parked = 1;
    return 0;
}

/* ---- TM_REQ_SYNC_WAKE handler -------------------------------------- */

int tm_sync_wake(pid_t caller, unsigned long addr, int max_n, unsigned mode)
{
    if (addr == 0) return -EINVAL;
    if (mode > TM_SYNC_WAKE_DISCARD) return -EINVAL;

    tm_sync_entry_t *e = find_entry(caller, addr);

    /* Always advance the gen counter for this (pid, addr).  Condvar
     * waiters compare against this. */
    if (e) e->gen += 1;

    if (max_n == 0) max_n = TM_SYNC_MAX_WAITERS;  /* 0 = wake all */
    int woken = 0;

    if (e) {
        while (woken < max_n) {
            seL4_CPtr slot = pop_waiter(e);
            if (!slot) break;
            /* Deliver a 0-status reply to the parked waiter. */
            seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);
            tm_reply_deliver(slot, tag, 0, 0, 0, 0);
            ++woken;
        }
    }

    if (woken < max_n && mode == TM_SYNC_WAKE_ABSORB) {
        /* Nobody (or not enough) to wake; deposit a single-shot
         * credit so the next WAIT in CREDIT mode consumes it
         * instead of parking.  We deposit ONE credit regardless of
         * how many additional wakes were "missed" — Sync* primitives
         * never need more than one absorbing wake outstanding. */
        if (!e) {
            e = alloc_entry(caller, addr);
            if (!e) return -ENOMEM;
            e->gen = 1;     /* this WAKE counts toward gen too */
        }
        if (e->credit < 1) e->credit = 1;
    }

    retire_if_idle(e);
    return 0;
}

/* On process exit, drop any per-process entries.  Called from the
 * process-reap path; no-op if the process never used Sync*. */
void tm_sync_pid_release(pid_t pid)
{
    for (int i = 0; i < TM_SYNC_MAX_ENTRIES; ++i) {
        if (!g_sync[i].in_use) continue;
        if (g_sync[i].pid != pid) continue;
        /* Drop any parked reply objects — the process is gone, so the
         * blocked callers will never be answered; delete the reply caps
         * and recycle the slots. */
        for (int w = 0; w < g_sync[i].nwaiters; ++w) {
            tm_reply_drop(g_sync[i].wait_slots[w]);
        }
        g_sync[i].in_use = 0;
        tm_warn("sync: dropped entry for terminated pid=%d addr=0x%lx",
                (int)pid, (unsigned long)g_sync[i].addr);
    }
}
