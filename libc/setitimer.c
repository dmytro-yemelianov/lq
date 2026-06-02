/*
 * setitimer.c — POSIX setitimer().
 *
 * Sends TM_REQ_SETITIMER with the (which, value-us, interval-us)
 * triple.  taskman stores the timer in tm_process_t and fires it
 * via SIGALRM pulse to the process's signal channel when the
 * deadline passes (sweep at every dispatch entry).
 *
 * v0.7 supports ITIMER_REAL only; VIRTUAL / PROF want per-process
 * CPU-time accounting we don't track yet — return EINVAL for those.
 * Time fields are converted to microseconds before crossing the
 * wire; us cover ~580k years in 64 bits.
 *
 * musl's alarm() calls setitimer(ITIMER_REAL, ...) — same shim
 * resolves both undefined refs.
 */

#include <sys/time.h>
#include <stddef.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

static inline unsigned long tv_to_us(const struct timeval *tv)
{
    return (unsigned long)tv->tv_sec * 1000000UL
         + (unsigned long)tv->tv_usec;
}

static inline void us_to_tv(unsigned long us, struct timeval *tv)
{
    tv->tv_sec  = (time_t)     (us / 1000000UL);
    tv->tv_usec = (suseconds_t)(us % 1000000UL);
}

int setitimer(int which, const struct itimerval *new_, struct itimerval *old)
{
    if (which != ITIMER_REAL) {
        /* VIRTUAL / PROF need per-process CPU-time accounting; v0.8. */
        qsoe_errno = EINVAL;
        return -1;
    }

    unsigned long value_us    = 0;
    unsigned long interval_us = 0;
    if (new_) {
        if (new_->it_value.tv_usec    < 0 || new_->it_value.tv_usec    >= 1000000L ||
            new_->it_interval.tv_usec < 0 || new_->it_interval.tv_usec >= 1000000L) {
            qsoe_errno = EINVAL;
            return -1;
        }
        value_us    = tv_to_us(&new_->it_value);
        interval_us = tv_to_us(&new_->it_interval);
    }

    seL4_Word mr0 = (seL4_Word)which;
    seL4_Word mr1 = (seL4_Word)value_us;
    seL4_Word mr2 = (seL4_Word)interval_us;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_SETITIMER, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    if (old) {
        us_to_tv((unsigned long)mr0, &old->it_value);
        us_to_tv((unsigned long)mr1, &old->it_interval);
    }
    return 0;
}
