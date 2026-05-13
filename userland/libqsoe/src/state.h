/*
 * libqsoe/src/state.h — per-process channel and connection bookkeeping.
 *
 * Internal to libqsoe. Application code should not include this.
 *
 * Lifecycle of a chid (and coid, symmetric):
 *   alloc       → reserves a chid index (slot value = QSOE_SLOT_RESERVED)
 *   bind(slot)  → stores the real recv-cap slot, marking it live
 *   bind(0)     → releases the chid back to the free pool
 */
#ifndef QSOE_LIBQSOE_STATE_H
#define QSOE_LIBQSOE_STATE_H

#include "../include/qsoe/qrv.h"

#define QSOE_MAX_CHANNELS    64
#define QSOE_MAX_CONNECTIONS 256

#define QSOE_SLOT_RESERVED  (~(unsigned long)0)

int qsoe_state_alloc_chid(void);
void qsoe_state_bind_chid(int chid, unsigned long slot);
unsigned long qsoe_state_chid_to_slot(int chid);

int qsoe_state_alloc_coid(void);
void qsoe_state_bind_coid(int coid, unsigned long slot);
unsigned long qsoe_state_coid_to_slot(int coid);

#endif /* QSOE_LIBQSOE_STATE_H */
