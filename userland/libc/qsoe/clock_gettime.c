/*
 * clock_gettime.c — POSIX clock_gettime().
 *
 * Thin shim over QNX-style ClockTime().  Splits the nsec result
 * into tv_sec / tv_nsec for the caller's struct timespec.
 */

#include <time.h>
#include <qsoe/qrv.h>

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    if (!ts) { qsoe_errno = EINVAL; return -1; }
    unsigned long nsec = 0;
    if (ClockTime(clk, 0, &nsec) != 0) return -1;
    ts->tv_sec  = (time_t)(nsec / 1000000000UL);
    ts->tv_nsec = (long)  (nsec % 1000000000UL);
    return 0;
}
