/*
 * nanosleep.c — POSIX nanosleep().
 *
 * Asks taskman to park us until a deadline.  taskman SaveCaller's
 * our reply slot and replies from its timer-sweep when rdtime
 * crosses the expiry.  The sweep runs at every TM_REQ_* dispatch
 * entry — granularity therefore depends on IPC load.  A future
 * tick-Notification (v0.8) or option-3 yield-poll thread tightens
 * the bound without changing this code.
 *
 * v0.7 simplification: the `rmtp` (remaining-time) outparam is
 * always zeroed.  Real signal-interrupt semantics arrive once the
 * signal thread is fully wired and EINTR can propagate back here.
 */

#include <time.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    if (rmtp) { rmtp->tv_sec = 0; rmtp->tv_nsec = 0; }
    if (!rqtp) { qsoe_errno = EFAULT; return -1; }
    if (rqtp->tv_nsec < 0 || rqtp->tv_nsec >= 1000000000L ||
        rqtp->tv_sec  < 0) {
        qsoe_errno = EINVAL;
        return -1;
    }

    /* Pack into a single 64-bit nanosecond count.  Caps at ~584
     * years (UINT64_MAX/1e9) — comfortable. */
    unsigned long total_ns = (unsigned long)rqtp->tv_sec * 1000000000UL
                           + (unsigned long)rqtp->tv_nsec;
    if (total_ns == 0) return 0;

    seL4_Word mr0 = (seL4_Word)total_ns, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_NANOSLEEP, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
}
