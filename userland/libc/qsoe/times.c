/*
 * times.c — POSIX times().
 *
 * Returns elapsed real time in clock ticks; fills *buf with the
 * caller's user / system / children user / children system times.
 *
 * v0.7 limitations:
 *   - QSOE doesn't yet account user vs system time per process —
 *     CLOCK_PROCESS_CPUTIME_ID is currently the same monotonic
 *     rdtime read every other clock observes.  We report it as
 *     tms_utime; tms_stime / cutime / cstime are zero.  This is
 *     POSIX-conforming (callers are told to expect any of the four
 *     slots may be zero on a given OS) — just not a useful split
 *     for profiling yet.
 *
 * Clock-ticks-per-second is _SC_CLK_TCK (= 100 in our sysconf).
 */

#include <sys/times.h>
#include <unistd.h>
#include <qsoe-system.h>

#define TMS_TICKS_PER_SEC  100UL
#define TMS_NSEC_PER_TICK  (1000000000UL / TMS_TICKS_PER_SEC)

static inline clock_t nsec_to_ticks(unsigned long nsec)
{
    return (clock_t)(nsec / TMS_NSEC_PER_TICK);
}

clock_t times(struct tms *buf)
{
    if (!buf) { qsoe_errno = EFAULT; return (clock_t)-1; }

    unsigned long cpu_ns  = 0;
    unsigned long real_ns = 0;
    if (ClockTime(CLOCK_PROCESS_CPUTIME_ID, 0, &cpu_ns) != 0) return (clock_t)-1;
    if (ClockTime(CLOCK_MONOTONIC,           0, &real_ns) != 0) return (clock_t)-1;

    buf->tms_utime  = nsec_to_ticks(cpu_ns);
    buf->tms_stime  = 0;
    buf->tms_cutime = 0;
    buf->tms_cstime = 0;
    return nsec_to_ticks(real_ns);
}
