/*
 * gettimeofday.c — POSIX gettimeofday().
 *
 * Thin shim over ClockTime(CLOCK_REALTIME).  Splits the nsec result
 * into tv_sec / tv_usec.  The `tz` argument is obsolete per POSIX
 * (struct timezone has been deprecated since 4.2BSD); we accept it
 * for ABI compatibility and ignore it.
 *
 * v0.7 note: CLOCK_REALTIME currently reports seconds-since-boot,
 * not seconds-since-epoch, because taskman doesn't yet track a
 * wall-clock offset.  When that lands the same code returns true
 * Unix time without changes here.
 */

#include <sys/time.h>
#include <qsoe-system.h>

int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    if (!tv) { qsoe_errno = EINVAL; return -1; }
    unsigned long nsec = 0;
    if (ClockTime(CLOCK_REALTIME, 0, &nsec) != 0) return -1;
    tv->tv_sec  = (time_t)     (nsec / 1000000000UL);
    tv->tv_usec = (suseconds_t)((nsec % 1000000000UL) / 1000UL);
    return 0;
}
