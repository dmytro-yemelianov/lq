/*
 * qsoe/timer.c -- LQ seam: SchedYield + the Timer* surface.
 *
 * SchedYield is real: seL4's SysYield (a debug-independent kernel
 * facility) donates the remaining timeslice and requeues the caller
 * at the tail of its priority -- the QNX SchedYield contract.
 *
 * TimerCreate / TimerDestroy manage per-process POSIX timer OBJECTS in
 * a libc-local table: a timer is allocated, validated (clock id, a
 * SIGEV_PULSE notify event, a live connection id), and freed.  This is
 * the create/destroy lifecycle; ARMING a timer (TimerSettime) and the
 * expiry-pulse delivery are a separate step -- they hang off taskman's
 * existing lazy sweep (proc/timer.c, the itimer/nanosleep path), so
 * TimerSettime/Info/Timeout stay announcing ENOSYS stubs for now.  The
 * stored sigevent makes each object ready for that future arm.
 * NQ's kernel-side implementation (nq/kernel/timer.c) is the shape
 * to mirror when arming lands.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <time.h>            /* CLOCK_REALTIME / CLOCK_MONOTONIC */
#include <sys/siginfo.h>     /* SIGEV_GET_TYPE, SIGEV_PULSE, sigev_coid */
#include <sys/qsoe.h>
#include "state.h"           /* qsoe_state_coid_to_slot, qsoe_spinlock_t */
#include "sel4_syscalls.h"   /* SEL4_SYS_YIELD, derived from the seL4 enum */

long SchedYield_r(void)
{
    register long _a7 __asm__("a7") = SEL4_SYS_YIELD;
    __asm__ volatile ("ecall" :: "r"(_a7) : "memory");
    return 0;
}

int SchedYield(void)
{
    (void) SchedYield_r();
    return 0;
}

/* Announce once per entry point, then keep returning ENOSYS --
 * the never-silent-stubs rule. */
#define TIMER_STUB_ANNOUNCE(name)                                        \
    do {                                                                 \
        static int announced;                                            \
        if (!announced) {                                                \
            announced = 1;                                               \
            qsoe_dbgprintf("STUB: " name " -> ENOSYS "                   \
                           "(LQ has no kernel timer surface yet)\n");    \
        }                                                                \
    } while (0)

/* Per-process POSIX timer objects.  The id handed to the caller is the
 * table index (>= 0, unique while live, reusable after TimerDestroy).
 * 32 is generous for a single process and keeps the exhaustion path
 * (EAGAIN) reachable. */
#define QSOE_TIMER_MAX 32

typedef struct {
    int             in_use;
    clockid_t       clockid;
    struct sigevent ev;        /* full copy: coid/code/prio for a future arm */
} qsoe_timer_t;

static qsoe_timer_t   g_timers[QSOE_TIMER_MAX];
static qsoe_spinlock_t g_timer_lock;

long TimerCreate_r(clockid_t id, const struct sigevent *event)
{
    /* Validate the notify event first (NULL / non-pulse).  Signals are
     * removed in QSOE, so only SIGEV_PULSE is accepted. */
    if (!event)                              return -EINVAL;
    if (SIGEV_GET_TYPE(event) != SIGEV_PULSE) return -EINVAL;
    /* Only the wall-clock and monotonic clocks exist (SOFTTIME aliases
     * REALTIME); CPU-time clocks and junk ids are rejected. */
    if (id != CLOCK_REALTIME && id != CLOCK_MONOTONIC) return -EINVAL;
    /* The pulse target connection must be live. */
    if (qsoe_state_coid_to_slot(event->sigev_coid) == 0) return -EBADF;

    qsoe_spin_lock(&g_timer_lock);
    int tid = -1;
    for (int i = 0; i < QSOE_TIMER_MAX; ++i) {
        if (!g_timers[i].in_use) { tid = i; break; }
    }
    if (tid < 0) { qsoe_spin_unlock(&g_timer_lock); return -EAGAIN; }
    g_timers[tid].in_use  = 1;
    g_timers[tid].clockid = id;
    g_timers[tid].ev      = *event;
    qsoe_spin_unlock(&g_timer_lock);
    return tid;
}

int TimerCreate(clockid_t id, const struct sigevent *event)
{
    long r = TimerCreate_r(id, event);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return (int) r;
}

long TimerDestroy_r(int tid)
{
    if (tid < 0 || tid >= QSOE_TIMER_MAX) return -EINVAL;
    qsoe_spin_lock(&g_timer_lock);
    if (!g_timers[tid].in_use) { qsoe_spin_unlock(&g_timer_lock); return -EINVAL; }
    g_timers[tid].in_use = 0;
    qsoe_spin_unlock(&g_timer_lock);
    return 0;
}

int TimerDestroy(int tid)
{
    long r = TimerDestroy_r(tid);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return 0;
}

long TimerSettime_r(int tid, int flags, const struct _itimer *itime,
                    struct _itimer *oitime)
{
    (void) tid; (void) flags; (void) itime; (void) oitime;
    TIMER_STUB_ANNOUNCE("TimerSettime");
    return -ENOSYS;
}

int TimerSettime(int tid, int flags, const struct _itimer *itime,
                 struct _itimer *oitime)
{
    long r = TimerSettime_r(tid, flags, itime, oitime);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return 0;
}

long TimerInfo_r(pid_t pid, int tid, int flags, struct _timer_info *info)
{
    (void) pid; (void) tid; (void) flags; (void) info;
    TIMER_STUB_ANNOUNCE("TimerInfo");
    return -ENOSYS;
}

int TimerInfo(pid_t pid, int tid, int flags, struct _timer_info *info)
{
    long r = TimerInfo_r(pid, tid, flags, info);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return 0;
}

long TimerTimeout_r(clockid_t id, int flags, const struct sigevent *notify,
                    const uint64_t *ntime, uint64_t *otime)
{
    (void) id; (void) flags; (void) notify; (void) ntime; (void) otime;
    TIMER_STUB_ANNOUNCE("TimerTimeout");
    return -ENOSYS;
}

long TimerTimeout(clockid_t id, int flags, const struct sigevent *notify,
                  const uint64_t *ntime, uint64_t *otime)
{
    long r = TimerTimeout_r(id, flags, notify, ntime, otime);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return r;
}
