/*
 * time.c — POSIX time().
 *
 * Returns the current CLOCK_REALTIME in seconds.  See gettimeofday.c
 * for the v0.7 caveat about CLOCK_REALTIME being seconds-since-boot
 * pending taskman's wall-clock offset.
 */

#include <time.h>
#include <qsoe/qrv.h>

time_t time(time_t *tloc)
{
    unsigned long nsec = 0;
    if (ClockTime(CLOCK_REALTIME, 0, &nsec) != 0) {
        if (tloc) *tloc = (time_t)-1;
        return (time_t)-1;
    }
    time_t sec = (time_t)(nsec / 1000000000UL);
    if (tloc) *tloc = sec;
    return sec;
}
