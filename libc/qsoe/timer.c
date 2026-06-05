/*
 * qsoe/timer.c -- LQ seam: SchedYield + the Timer* surface.
 *
 * SchedYield is real: seL4's SysYield (a debug-independent kernel
 * facility) donates the remaining timeslice and requeues the caller
 * at the tail of its priority -- the QNX SchedYield contract.
 *
 * The Timer* family is announcing ENOSYS stubs: kernel-tracked
 * timers need a tick + expiry-pulse story that LQ doesn't have yet
 * (stock seL4 has no user timer surface; the MCS switch planned for
 * v1.0 brings sched contexts + timeouts).  The symbols must still
 * exist so the SHARED userspace -- one suite binary for both
 * kernels -- links and runs; the timer tests then FAIL loudly at
 * runtime instead of the build dying or, worse, a silent no-op.
 * NQ's kernel-side implementation (nq/kernel/timer.c) is the shape
 * to mirror when this lands.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <sys/qsoe.h>

/* seL4 RISC-V syscall ABI: a7 = syscall number.  SysYield is -7 --
 * keep in step with the generated arch/api/syscall.h. */
#define LQ_SEL4_SYS_YIELD  (-7)

long SchedYield_r(void)
{
    register long _a7 __asm__("a7") = LQ_SEL4_SYS_YIELD;
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

long TimerCreate_r(clockid_t id, const struct sigevent *event)
{
    (void) id; (void) event;
    TIMER_STUB_ANNOUNCE("TimerCreate");
    return -ENOSYS;
}

int TimerCreate(clockid_t id, const struct sigevent *event)
{
    long r = TimerCreate_r(id, event);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return (int) r;
}

long TimerDestroy_r(int tid)
{
    (void) tid;
    TIMER_STUB_ANNOUNCE("TimerDestroy");
    return -ENOSYS;
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
