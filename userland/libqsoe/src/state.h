/*
 * libqsoe/src/state.h — per-process channel/connection bookkeeping.
 *
 * Internal to libqsoe. Application code should not include this.
 *
 * v0.3.3 split each pool in two:
 *   - FD pool       : low (1-based) handles, share the int namespace
 *                     with file descriptors once we have a libc.
 *   - side-channel  : QSOE_SIDE_CHANNEL | index handles (bit 30 set).
 *                     Used by library/kernel-internal connections —
 *                     most importantly SYSMGR_COID (= the very first
 *                     side-channel coid, pre-bound at process start).
 *
 * The slot value 0 means "free", QSOE_SLOT_RESERVED means "allocated
 * but not yet bound to a real cap slot"; anything else is a live
 * seL4 CPtr in this process's CSpace.
 */
#ifndef QSOE_LIBQSOE_STATE_H
#define QSOE_LIBQSOE_STATE_H

#include "../include/qsoe/qrv.h"

#define QSOE_MAX_FD_CHANNELS     64
#define QSOE_MAX_SIDE_CHANNELS   16
#define QSOE_MAX_FD_CONNECTIONS 256
#define QSOE_MAX_SIDE_CONNECTIONS 16

#define QSOE_SLOT_RESERVED  (~(unsigned long)0)

/* qsoe_self_pid is now a macro that reads from per-thread state —
 * see <qsoe/tls.h>. Still the same name and semantics; just stored in
 * the qsoe_tcb_t instead of a global. */

/* `flags` selects pool: bit QSOE_SIDE_CHANNEL → side pool, else FD. */
int           qsoe_state_alloc_chid(unsigned flags);
void          qsoe_state_bind_chid(int chid, unsigned long slot);
unsigned long qsoe_state_chid_to_slot(int chid);

int           qsoe_state_alloc_coid(unsigned flags);
void          qsoe_state_bind_coid(int coid, unsigned long slot);
unsigned long qsoe_state_coid_to_slot(int coid);
void          qsoe_state_force_bind_coid(int coid, unsigned long slot);

/* v0.4 thread pool accessors. */
qsoe_tcb_t   *qsoe_tcb_of_tid(int tid);
qsoe_tcb_t   *qsoe_worker_alloc(void);

extern qsoe_tcb_t qsoe_worker_tcbs[31];

/* v0.6.4: empty-CSpace-slot allocator for cap-receive paths from
 * inside the process (currently: SaveCaller in resmgr park
 * patterns). Returns 0 on exhaustion. */
unsigned long qsoe_state_alloc_empty_slot(void);
void          qsoe_state_free_empty_slot(unsigned long slot);

#endif /* QSOE_LIBQSOE_STATE_H */
