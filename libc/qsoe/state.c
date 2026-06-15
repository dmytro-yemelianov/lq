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
/* Global channels (QSOE_CHF_GLOBAL): taskman assigns an arbitrary
 * system-unique chid (QSOE_GLOBAL_CHANNEL | counter), so we can't index it
 * by value -- keep a small (chid -> recv slot) association the owner uses
 * for MsgReceive.  A process owns few global channels. */
static struct { int chid; unsigned long slot; }
                     g_global_chid    [QSOE_MAX_GLOBAL_CHANNELS];
static unsigned long g_fd_coid_slot   [QSOE_MAX_FD_CONNECTIONS];
static unsigned long g_side_coid_slot [QSOE_MAX_SIDE_CONNECTIONS];
/* v0.7 per-fd flags for POSIX fcntl(F_GETFD/SETFD/GETFL/SETFL).
 * One 32-bit word per fd-pool entry; side-channel coids don't use
 * flags so no parallel array there. */
static unsigned       g_fd_coid_flags  [QSOE_MAX_FD_CONNECTIONS];

static qsoe_spinlock_t g_state_lock;

/* The per-process main thread's TCB record. Lives in BSS at a known
 * address so the crt0 can load &qsoe_main_tcb into tp. tid is 1; the
 * rest is filled by qsoe_libc_init() at process startup. */
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
static inline int is_global(int handle) {
    return QSOE_IS_GLOBAL_CHANNEL(handle) != 0;
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
    if (is_global(chid)) {
        /* Find an existing entry for this chid, else a free one.  slot==0
         * (unbind) clears the entry. */
        int free = -1;
        for (int i = 0; i < QSOE_MAX_GLOBAL_CHANNELS; ++i) {
            if (g_global_chid[i].chid == chid) { free = i; break; }
            if (free < 0 && g_global_chid[i].chid == 0) free = i;
        }
        if (free >= 0) {
            g_global_chid[free].chid = slot ? chid : 0;
            g_global_chid[free].slot = slot;
        }
    } else if (is_side(chid)) {
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
    if (is_global(chid)) {
        for (int i = 0; i < QSOE_MAX_GLOBAL_CHANNELS; ++i)
            if (g_global_chid[i].chid == chid) { s = g_global_chid[i].slot; break; }
    } else if (is_side(chid)) {
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
        /* fd=0 is permitted (stdin). qsoe_state_alloc_coid scans from
         * i=1, so it never auto-allocates 0; force-binding via
         * qsoe_state_force_bind_coid is the only path that uses it. */
        if (coid >= 0 && coid < QSOE_MAX_FD_CONNECTIONS) g_fd_coid_slot[coid] = slot;
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
        if (coid >= 0 && coid < QSOE_MAX_FD_CONNECTIONS) s = g_fd_coid_slot[coid];
    }
    qsoe_spin_unlock(&g_state_lock);
    return (s == QSOE_SLOT_RESERVED) ? 0 : s;
}

/* v0.5.0: bind an FD-namespace coid (including 0) directly to a
 * CSpace slot, bypassing the alloc_coid path. Used at process
 * startup to wire fds 0/1/2 to the inherited stdio connections that
 * spawn.c minted into QSOE_CAP_STDIN/OUT/ERR_CONNECT. After this
 * call, qsoe_state_alloc_coid will not return `coid` because the
 * slot is non-zero. */
void qsoe_state_force_bind_coid(int coid, unsigned long slot)
{
    qsoe_state_bind_coid(coid, slot);
}

unsigned qsoe_state_get_coid_flags(int coid)
{
    unsigned out = 0;
    qsoe_spin_lock(&g_state_lock);
    if (!is_side(coid) && coid >= 0 && coid < QSOE_MAX_FD_CONNECTIONS) {
        out = g_fd_coid_flags[coid];
    }
    qsoe_spin_unlock(&g_state_lock);
    return out;
}

void qsoe_state_set_coid_flags(int coid, unsigned flags)
{
    qsoe_spin_lock(&g_state_lock);
    if (!is_side(coid) && coid >= 0 && coid < QSOE_MAX_FD_CONNECTIONS) {
        g_fd_coid_flags[coid] = flags;
    }
    qsoe_spin_unlock(&g_state_lock);
}

int qsoe_state_alloc_coid_ge(int start)
{
    int out = -1;
    if (start < 0) start = 0;
    if (start >= QSOE_MAX_FD_CONNECTIONS) return -1;
    qsoe_spin_lock(&g_state_lock);
    for (int i = start; i < QSOE_MAX_FD_CONNECTIONS; ++i) {
        if (g_fd_coid_slot[i] == 0) {
            g_fd_coid_slot[i]  = QSOE_SLOT_RESERVED;
            g_fd_coid_flags[i] = 0;
            out = i;
            break;
        }
    }
    qsoe_spin_unlock(&g_state_lock);
    return out;
}

/* Empty-CSpace-slot allocator.  Returns a fresh CPtr in the caller's
 * own CSpace, suitable as a destination for a stashed reply object
 * (deferred reply) or other cap-receive paths from inside the process.
 * Range starts at 0x800 — well past the chid/coid pools, well inside
 * the child's 4096-slot CNode.  A Send on the stashed reply object
 * consumes it once; the slot is then handed back via _free_ for reuse. */
#define QSOE_EMPTY_SLOT_BASE  0x800UL
#define QSOE_EMPTY_SLOT_MAX   0x1000UL
#define QSOE_EMPTY_SLOT_FREE_MAX 16

static unsigned long g_next_empty_slot = QSOE_EMPTY_SLOT_BASE;
static unsigned long g_empty_slot_free[QSOE_EMPTY_SLOT_FREE_MAX];
static int g_empty_slot_free_count;

unsigned long qsoe_state_alloc_empty_slot(void);
unsigned long qsoe_state_alloc_empty_slot(void)
{
    unsigned long out = 0;
    qsoe_spin_lock(&g_state_lock);
    if (g_empty_slot_free_count > 0) {
        out = g_empty_slot_free[--g_empty_slot_free_count];
    } else if (g_next_empty_slot < QSOE_EMPTY_SLOT_MAX) {
        out = g_next_empty_slot++;
    }
    qsoe_spin_unlock(&g_state_lock);
    return out;
}

void qsoe_state_free_empty_slot(unsigned long slot);
void qsoe_state_free_empty_slot(unsigned long slot)
{
    if (slot == 0) return;
    qsoe_spin_lock(&g_state_lock);
    if (g_empty_slot_free_count < QSOE_EMPTY_SLOT_FREE_MAX) {
        g_empty_slot_free[g_empty_slot_free_count++] = slot;
    }
    qsoe_spin_unlock(&g_state_lock);
}
