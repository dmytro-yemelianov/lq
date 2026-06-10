/*
 * nanosleep.c — POSIX nanosleep() (QSOE/L).
 *
 * Client-side timed spin against the RISC-V `time` CSR.
 *
 * The earlier design parked the caller in taskman (TM_REQ_NANOSLEEP) and
 * relied on taskman's timer sweep to reply when rdtime crossed the
 * expiry.  But that sweep only runs when *something* pokes taskman, so a
 * sleeper on an otherwise-idle system (e.g. sysinfo measuring load while
 * the shell waits on it) never woke -- a deadlock.  On RISC-V MCS the
 * kernel owns the timer for scheduling, so user-level has no timer IRQ
 * to drive a wake; until a budget-throttled tick thread or a
 * tick-Notification lands in taskman, sleep by polling rdtime here and
 * yielding each turn so equal-priority threads still make progress.
 *
 * rdtime is U-mode readable per the RISC-V spec; qsoe_time_freq_hz was
 * cached from TM_REQ_CLOCK_FREQ at process startup.
 *
 * The `rmtp` (remaining-time) outparam is always zeroed.  Real
 * signal-interrupt (EINTR) semantics arrive once the signal thread can
 * unblock a spinning sleeper.
 */

#include <time.h>
#include <sys/qsoe.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

/* RISC-V `time` CSR — current tick count (U-mode allowed by spec). */
static inline unsigned long nsleep_rdtime(void)
{
    unsigned long t;
    __asm__ volatile ("rdtime %0" : "=r"(t));
    return t;
}

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    if (rmtp) { rmtp->tv_sec = 0; rmtp->tv_nsec = 0; }
    if (!rqtp) { qsoe_errno = EFAULT; return -1; }
    if (rqtp->tv_nsec < 0 || rqtp->tv_nsec >= 1000000000L ||
        rqtp->tv_sec  < 0) {
        qsoe_errno = EINVAL;
        return -1;
    }

    /* Pack into a single 64-bit nanosecond count.  Caps at ~584 years
     * (UINT64_MAX/1e9) -- comfortable. */
    unsigned long total_ns = (unsigned long)rqtp->tv_sec * 1000000000UL
                           + (unsigned long)rqtp->tv_nsec;
    if (total_ns == 0) return 0;

    /* No clock frequency -> can't time the wait; best-effort no-op
     * rather than spin forever. */
    if (qsoe_time_freq_hz == 0) return 0;

    /* ns -> ticks, split to avoid a 128-bit multiply. */
    unsigned long freq  = qsoe_time_freq_hz;
    unsigned long whole = (total_ns / 1000000000UL) * freq;
    unsigned long part  = ((total_ns % 1000000000UL) * freq) / 1000000000UL;
    unsigned long target = nsleep_rdtime() + whole + part;

    while ((long)(nsleep_rdtime() - target) < 0)
        qsoe_sys_yield();

    return 0;
}
