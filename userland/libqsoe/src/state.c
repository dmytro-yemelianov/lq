/*
 * libqsoe/src/state.c — per-process chid and coid bookkeeping.
 *
 * v0.3.3: split each pool into an FD-range table (1..N) and a
 * side-channel table indexed by (handle & ~QSOE_SIDE_CHANNEL).
 *
 * v0.4: SMP-safe via a coarse per-process spinlock guarding every
 * pool and the worker-thread allocator. Refined to per-pool locks
 * if/when contention is measured.
 */

#include "state.h"

static unsigned long g_fd_chid_slot   [QSOE_MAX_FD_CHANNELS];
static unsigned long g_side_chid_slot [QSOE_MAX_SIDE_CHANNELS];
static unsigned long g_fd_coid_slot   [QSOE_MAX_FD_CONNECTIONS];
static unsigned long g_side_coid_slot [QSOE_MAX_SIDE_CONNECTIONS];

static qsoe_spinlock_t g_state_lock;

/* The per-process main thread's TCB record. Lives in BSS at a known
 * address so the crt0 can load &qsoe_main_tcb into tp. tid is 1; the
 * rest is filled by qsoe_libqsoe_init() at process startup. */
qsoe_tcb_t qsoe_main_tcb = { .tid = 1 };

/* Worker thread state pool — tids 2..32. Indexed by tid-2. */
qsoe_tcb_t qsoe_worker_tcbs[31];

/* Monotonic allocator: slot i is given out exactly once (so its vaddr
 * range stays exclusively assigned). Reuse is deferred to v0.4.1+,
 * which adds VSpace cleanup on ThreadDestroy. After Join/Detach the
 * `reaped` flag hides the slot from lookups but leaves vaddrs claimed. */
static int g_next_worker_idx = 0;

qsoe_tcb_t *qsoe_tcb_of_tid(int tid)
{
    if (tid == 1) return &qsoe_main_tcb;  /* read-only, no lock needed */
    qsoe_tcb_t *out = 0;
    qsoe_spin_lock(&g_state_lock);
    if (tid >= 2 && tid <= 32) {
        qsoe_tcb_t *t = &qsoe_worker_tcbs[tid - 2];
        if (t->tid == tid && !t->reaped) out = t;
    }
    qsoe_spin_unlock(&g_state_lock);
    return out;
}

qsoe_tcb_t *qsoe_worker_alloc(void)
{
    qsoe_tcb_t *out = 0;
    qsoe_spin_lock(&g_state_lock);
    if (g_next_worker_idx < 31) {
        out = &qsoe_worker_tcbs[g_next_worker_idx];
        out->tid = g_next_worker_idx + 2;
        g_next_worker_idx++;
    }
    qsoe_spin_unlock(&g_state_lock);
    return out;
}

static inline int is_side(int handle) {
    return ((unsigned)handle & QSOE_SIDE_CHANNEL) != 0;
}
static inline int side_index(int handle) {
    return (int)((unsigned)handle & ~QSOE_SIDE_CHANNEL);
}

/* ---------------- chid ---------------- */

int qsoe_state_alloc_chid(unsigned flags)
{
    int out = -1;
    qsoe_spin_lock(&g_state_lock);
    if (flags & QSOE_SIDE_CHANNEL) {
        for (int i = 0; i < QSOE_MAX_SIDE_CHANNELS; ++i) {
            if (g_side_chid_slot[i] == 0) {
                g_side_chid_slot[i] = QSOE_SLOT_RESERVED;
                out = (int)(QSOE_SIDE_CHANNEL | (unsigned)i);
                break;
            }
        }
    } else {
        for (int i = 1; i < QSOE_MAX_FD_CHANNELS; ++i) {
            if (g_fd_chid_slot[i] == 0) {
                g_fd_chid_slot[i] = QSOE_SLOT_RESERVED;
                out = i;
                break;
            }
        }
    }
    qsoe_spin_unlock(&g_state_lock);
    return out;
}

void qsoe_state_bind_chid(int chid, unsigned long slot)
{
    qsoe_spin_lock(&g_state_lock);
    if (is_side(chid)) {
        int i = side_index(chid);
        if (i >= 0 && i < QSOE_MAX_SIDE_CHANNELS) g_side_chid_slot[i] = slot;
    } else {
        if (chid >= 1 && chid < QSOE_MAX_FD_CHANNELS) g_fd_chid_slot[chid] = slot;
    }
    qsoe_spin_unlock(&g_state_lock);
}

unsigned long qsoe_state_chid_to_slot(int chid)
{
    unsigned long s = 0;
    qsoe_spin_lock(&g_state_lock);
    if (is_side(chid)) {
        int i = side_index(chid);
        if (i >= 0 && i < QSOE_MAX_SIDE_CHANNELS) s = g_side_chid_slot[i];
    } else {
        if (chid >= 1 && chid < QSOE_MAX_FD_CHANNELS) s = g_fd_chid_slot[chid];
    }
    qsoe_spin_unlock(&g_state_lock);
    return (s == QSOE_SLOT_RESERVED) ? 0 : s;
}

/* ---------------- coid ---------------- */

int qsoe_state_alloc_coid(unsigned flags)
{
    int out = -1;
    qsoe_spin_lock(&g_state_lock);
    if (flags & QSOE_SIDE_CHANNEL) {
        for (int i = 0; i < QSOE_MAX_SIDE_CONNECTIONS; ++i) {
            if (g_side_coid_slot[i] == 0) {
                g_side_coid_slot[i] = QSOE_SLOT_RESERVED;
                out = (int)(QSOE_SIDE_CHANNEL | (unsigned)i);
                break;
            }
        }
    } else {
        for (int i = 1; i < QSOE_MAX_FD_CONNECTIONS; ++i) {
            if (g_fd_coid_slot[i] == 0) {
                g_fd_coid_slot[i] = QSOE_SLOT_RESERVED;
                out = i;
                break;
            }
        }
    }
    qsoe_spin_unlock(&g_state_lock);
    return out;
}

void qsoe_state_bind_coid(int coid, unsigned long slot)
{
    qsoe_spin_lock(&g_state_lock);
    if (is_side(coid)) {
        int i = side_index(coid);
        if (i >= 0 && i < QSOE_MAX_SIDE_CONNECTIONS) g_side_coid_slot[i] = slot;
    } else {
        if (coid >= 1 && coid < QSOE_MAX_FD_CONNECTIONS) g_fd_coid_slot[coid] = slot;
    }
    qsoe_spin_unlock(&g_state_lock);
}

unsigned long qsoe_state_coid_to_slot(int coid)
{
    unsigned long s = 0;
    qsoe_spin_lock(&g_state_lock);
    if (is_side(coid)) {
        int i = side_index(coid);
        if (i >= 0 && i < QSOE_MAX_SIDE_CONNECTIONS) s = g_side_coid_slot[i];
    } else {
        if (coid >= 1 && coid < QSOE_MAX_FD_CONNECTIONS) s = g_fd_coid_slot[coid];
    }
    qsoe_spin_unlock(&g_state_lock);
    return (s == QSOE_SLOT_RESERVED) ? 0 : s;
}
