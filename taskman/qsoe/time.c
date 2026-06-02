/*
 * time.c — QNX-compatible Clock* implementation, backed by RISC-V's
 * `rdtime` CSR plus a process-wide cached frequency.
 *
 * Boot path: _qsoe_start_main (spawned processes) issues TM_REQ_CLOCK_FREQ
 * once and stores the reply into qsoe_time_freq_hz.  taskman itself
 * assigns it directly from sys/platform.h's TM_CLOCK_FREQ_HZ, no IPC.
 * Future v0.8 work moves the constant to a runtime FDT query
 * (see [[project_clock_freq_fdt_v08.md]] in memory).
 *
 * For the conversion ticks → ns we use __int128 to avoid overflow:
 * `ticks * 1e9` doesn't fit in 64 bits past ~18 seconds of monotonic
 * time at 10 MHz.  All other arithmetic is plain 64-bit.
 */

#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>

#include "sel4_types.h"
#include "qsoe_invoke.h"

unsigned long qsoe_time_freq_hz;   /* populated at startup */

static inline unsigned long rdtime(void)
{
    unsigned long t;
    __asm__ volatile ("rdtime %0" : "=r"(t));
    return t;
}

/* ticks * 1e9 / freq, split to avoid 64-bit overflow at the
 * multiply step:
 *   nsec = (ticks / freq) * 1e9 + ((ticks % freq) * 1e9) / freq
 * Both terms fit in uint64 for any plausible freq (kHz..GHz). */
static inline unsigned long ticks_to_nsec(unsigned long ticks)
{
    if (qsoe_time_freq_hz == 0) return 0;
    unsigned long whole = (ticks / qsoe_time_freq_hz) * 1000000000UL;
    unsigned long part  = ((ticks % qsoe_time_freq_hz) * 1000000000UL)
                          / qsoe_time_freq_hz;
    return whole + part;
}

/* One-shot IPC to taskman; called from _qsoe_start_main exactly once. */
int qsoe_query_clock_freq(unsigned long *out_hz);
int qsoe_query_clock_freq(unsigned long *out_hz)
{
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_CLOCK_FREQ, 0, 0, 0);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    *out_hz = (unsigned long)mr0;
    return 0;
}

/* ----------- ClockCycles: raw rdtime ----------- */

unsigned long ClockCycles(void)
{
    return rdtime();
}

/* ----------- ClockTime: read / set (nsec) ----------- */

int ClockTime(clockid_t id, const unsigned long *new_, unsigned long *old)
{
    /* All five QNX clock IDs collapse to rdtime in v0.7 — we don't
     * yet distinguish wall clock from monotonic from CPU-time, and
     * there is only one system clock.  When v0.8 adds a wall-clock
     * offset and per-process CPU accounting, this routes by id. */
    (void)id;
    if (old) *old = ticks_to_nsec(rdtime());
    if (new_) {
        /* Setting the clock needs taskman to hold an offset for
         * CLOCK_REALTIME.  Not yet wired; surface as the POSIX-correct
         * permission failure rather than silently dropping the write. */
        qsoe_errno = EPERM;
        return -1;
    }
    return 0;
}

/* ----------- ClockAdjust: not yet implemented ----------- */

int ClockAdjust(clockid_t id, const struct _clockadjust *new_,
                struct _clockadjust *old)
{
    (void)id; (void)new_; (void)old;
    /* Needs taskman-side tick accumulator state.  Read it back as
     * zero is misleading; ENOSYS is the honest answer. */
    qsoe_errno = ENOSYS;
    return -1;
}

/* ----------- ClockPeriod: period (ns/tick) of the named clock ----------- */

int ClockPeriod(clockid_t id, const struct _clockperiod *new_,
                struct _clockperiod *old, int reserved)
{
    (void)id; (void)reserved;
    if (old) {
        if (qsoe_time_freq_hz == 0) {
            old->nsec  = 0;
            old->fract = 0;
        } else {
            /* nsec = 1e9 / freq; fract = remainder as 32-bit fixed-point. */
            old->nsec  = (unsigned int)(1000000000UL / qsoe_time_freq_hz);
            unsigned long rem = 1000000000UL % qsoe_time_freq_hz;
            old->fract = (int)((rem << 32) / qsoe_time_freq_hz);
        }
    }
    if (new_) {
        /* QSOE doesn't let userspace reprogram the system clock. */
        qsoe_errno = EPERM;
        return -1;
    }
    return 0;
}

/* ----------- ClockId: clock id for a (pid, tid) ----------- */

int ClockId(pid_t pid, int tid)
{
    /* QNX returns a clock id you can pass to ClockTime / TimerCreate.
     * In v0.7 every process / thread shares the one system clock; we
     * return CLOCK_PROCESS_CPUTIME_ID for the named target if it
     * exists, ESRCH otherwise.  tid=0 means "the process as a whole". */
    (void)tid;
    if (pid == 0) return CLOCK_PROCESS_CPUTIME_ID;
    /* No cross-process visibility yet — accept the caller's own pid
     * (qsoe_curthr()->self_pid), reject others. */
    extern qsoe_tcb_t *qsoe_curthr(void);
    if (pid == qsoe_curthr()->self_pid) return CLOCK_PROCESS_CPUTIME_ID;
    qsoe_errno = ESRCH;
    return -1;
}
