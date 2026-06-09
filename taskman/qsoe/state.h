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
 *                     most importantly TASKMAN_COID (= the very first
 *                     side-channel coid, pre-bound at process start).
 *
 * The slot value 0 means "free", QSOE_SLOT_RESERVED means "allocated
 * but not yet bound to a real cap slot"; anything else is a live
 * seL4 CPtr in this process's CSpace.
 */
#ifndef QSOE_LIBQSOE_STATE_H
#define QSOE_LIBQSOE_STATE_H

#include <sys/qsoe.h>
#include <qsoe/tls.h>      /* qsoe_spinlock_t, qsoe_main_tcb, qsoe_curthr */

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

/* v0.7 per-fd flag bag, backing POSIX fcntl(F_GETFD/F_SETFD/F_GETFL/
 * F_SETFL).  One 32-bit word per fd in the FD pool; side-channel
 * coids don't expose flags (they aren't user-facing).  Zero on
 * alloc; cleared when the coid is unbound. */
unsigned      qsoe_state_get_coid_flags(int coid);
void          qsoe_state_set_coid_flags(int coid, unsigned flags);

/* Lowest free fd ≥ `start` for fcntl(F_DUPFD).  Returns the fd or
 * -1 if the pool is exhausted.  The slot is left RESERVED so a
 * subsequent bind doesn't race against alloc_coid. */
int           qsoe_state_alloc_coid_ge(int start);

/* v0.4 thread pool accessors. */
qsoe_tcb_t   *qsoe_tcb_of_tid(int tid);
qsoe_tcb_t   *qsoe_worker_alloc(void);

extern qsoe_tcb_t qsoe_worker_tcbs[31];

/* Empty-CSpace-slot allocator for cap-receive paths from inside the
 * process (e.g. stashing a reply object when a resmgr parks a deferred
 * reply). Returns 0 on exhaustion. */
unsigned long qsoe_state_alloc_empty_slot(void);
void          qsoe_state_free_empty_slot(unsigned long slot);

#endif /* QSOE_LIBQSOE_STATE_H */
