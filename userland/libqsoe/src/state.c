/*
 * libqsoe/src/state.c — per-process chid and coid bookkeeping.
 *
 * v0.3.3: split each pool into an FD-range table (1..N) and a
 * side-channel table indexed by (handle & ~QSOE_SIDE_CHANNEL).
 */

#include "state.h"

static unsigned long g_fd_chid_slot   [QSOE_MAX_FD_CHANNELS];
static unsigned long g_side_chid_slot [QSOE_MAX_SIDE_CHANNELS];
static unsigned long g_fd_coid_slot   [QSOE_MAX_FD_CONNECTIONS];
static unsigned long g_side_coid_slot [QSOE_MAX_SIDE_CONNECTIONS];

int qsoe_errno;
pid_t qsoe_self_pid;

static inline int is_side(int handle) {
    return ((unsigned)handle & QSOE_SIDE_CHANNEL) != 0;
}
static inline int side_index(int handle) {
    return (int)((unsigned)handle & ~QSOE_SIDE_CHANNEL);
}

/* ---------------- chid ---------------- */

int qsoe_state_alloc_chid(unsigned flags)
{
    if (flags & QSOE_SIDE_CHANNEL) {
        for (int i = 0; i < QSOE_MAX_SIDE_CHANNELS; ++i) {
            if (g_side_chid_slot[i] == 0) {
                g_side_chid_slot[i] = QSOE_SLOT_RESERVED;
                return (int)(QSOE_SIDE_CHANNEL | (unsigned)i);
            }
        }
        return -1;
    }
    for (int i = 1; i < QSOE_MAX_FD_CHANNELS; ++i) {
        if (g_fd_chid_slot[i] == 0) {
            g_fd_chid_slot[i] = QSOE_SLOT_RESERVED;
            return i;
        }
    }
    return -1;
}

void qsoe_state_bind_chid(int chid, unsigned long slot)
{
    if (is_side(chid)) {
        int i = side_index(chid);
        if (i >= 0 && i < QSOE_MAX_SIDE_CHANNELS) g_side_chid_slot[i] = slot;
    } else {
        if (chid >= 1 && chid < QSOE_MAX_FD_CHANNELS) g_fd_chid_slot[chid] = slot;
    }
}

unsigned long qsoe_state_chid_to_slot(int chid)
{
    unsigned long s = 0;
    if (is_side(chid)) {
        int i = side_index(chid);
        if (i >= 0 && i < QSOE_MAX_SIDE_CHANNELS) s = g_side_chid_slot[i];
    } else {
        if (chid >= 1 && chid < QSOE_MAX_FD_CHANNELS) s = g_fd_chid_slot[chid];
    }
    return (s == QSOE_SLOT_RESERVED) ? 0 : s;
}

/* ---------------- coid ---------------- */

int qsoe_state_alloc_coid(unsigned flags)
{
    if (flags & QSOE_SIDE_CHANNEL) {
        for (int i = 0; i < QSOE_MAX_SIDE_CONNECTIONS; ++i) {
            if (g_side_coid_slot[i] == 0) {
                g_side_coid_slot[i] = QSOE_SLOT_RESERVED;
                return (int)(QSOE_SIDE_CHANNEL | (unsigned)i);
            }
        }
        return -1;
    }
    for (int i = 1; i < QSOE_MAX_FD_CONNECTIONS; ++i) {
        if (g_fd_coid_slot[i] == 0) {
            g_fd_coid_slot[i] = QSOE_SLOT_RESERVED;
            return i;
        }
    }
    return -1;
}

void qsoe_state_bind_coid(int coid, unsigned long slot)
{
    if (is_side(coid)) {
        int i = side_index(coid);
        if (i >= 0 && i < QSOE_MAX_SIDE_CONNECTIONS) g_side_coid_slot[i] = slot;
    } else {
        if (coid >= 1 && coid < QSOE_MAX_FD_CONNECTIONS) g_fd_coid_slot[coid] = slot;
    }
}

unsigned long qsoe_state_coid_to_slot(int coid)
{
    unsigned long s = 0;
    if (is_side(coid)) {
        int i = side_index(coid);
        if (i >= 0 && i < QSOE_MAX_SIDE_CONNECTIONS) s = g_side_coid_slot[i];
    } else {
        if (coid >= 1 && coid < QSOE_MAX_FD_CONNECTIONS) s = g_fd_coid_slot[coid];
    }
    return (s == QSOE_SLOT_RESERVED) ? 0 : s;
}
