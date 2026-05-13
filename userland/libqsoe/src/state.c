/*
 * libqsoe/src/state.c — per-process chid and coid bookkeeping.
 *
 * Flat arrays for v0.x. chid/coid are 1-based handles. Slot value 0
 * means "free", QSOE_SLOT_RESERVED means "allocated but not yet bound
 * to a real slot", anything else is a live cap slot.
 */

#include "state.h"

static unsigned long g_chid_slot[QSOE_MAX_CHANNELS];
static unsigned long g_coid_slot[QSOE_MAX_CONNECTIONS];

int qsoe_errno;

int qsoe_state_alloc_chid(void)
{
    for (int i = 1; i < QSOE_MAX_CHANNELS; ++i) {
        if (g_chid_slot[i] == 0) {
            g_chid_slot[i] = QSOE_SLOT_RESERVED;
            return i;
        }
    }
    return -1;
}

void qsoe_state_bind_chid(int chid, unsigned long slot)
{
    if (chid >= 1 && chid < QSOE_MAX_CHANNELS) g_chid_slot[chid] = slot;
}

unsigned long qsoe_state_chid_to_slot(int chid)
{
    if (chid < 1 || chid >= QSOE_MAX_CHANNELS) return 0;
    unsigned long s = g_chid_slot[chid];
    return (s == QSOE_SLOT_RESERVED) ? 0 : s;
}

int qsoe_state_alloc_coid(void)
{
    for (int i = 1; i < QSOE_MAX_CONNECTIONS; ++i) {
        if (g_coid_slot[i] == 0) {
            g_coid_slot[i] = QSOE_SLOT_RESERVED;
            return i;
        }
    }
    return -1;
}

void qsoe_state_bind_coid(int coid, unsigned long slot)
{
    if (coid >= 1 && coid < QSOE_MAX_CONNECTIONS) g_coid_slot[coid] = slot;
}

unsigned long qsoe_state_coid_to_slot(int coid)
{
    if (coid < 1 || coid >= QSOE_MAX_CONNECTIONS) return 0;
    unsigned long s = g_coid_slot[coid];
    return (s == QSOE_SLOT_RESERVED) ? 0 : s;
}
