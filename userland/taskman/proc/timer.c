/*
 * proc/timer.c — v0.7 hybrid timer infrastructure.
 *
 * Backs POSIX nanosleep + setitimer (and through them: sleep,
 * usleep, alarm).  Strategy: every TM_REQ_* dispatch entry calls
 * tm_timer_sweep() which:
 *
 *   1. Reads rdtime (current ticks).
 *   2. Walks g_sleepers[] — for each parked nanosleep whose
 *      expiry has passed, delivers a 0-status reply to the
 *      saved reply cap and frees the slot.
 *   3. Walks g_processes[] — for each process whose ITIMER_REAL
 *      is armed and expired, sends a SIGALRM pulse on its
 *      signal_chid channel and re-arms (or disarms) per the
 *      stored interval.
 *
 * Lazy: timers only fire when *something* pokes taskman.  Quiet
 * systems get coarse granularity; IPC-busy ones see real-time
 * timer behaviour.  Adding a yield-poll thread or, eventually, the
 * kernel tick-notification patch tightens latency without touching
 * the wire protocol or this file's API.
 */

#include "proc.h"
#include "../qsoe_invoke.h"

#define TM_MAX_SLEEPERS 16

typedef struct {
    int            in_use;
    pid_t          caller_pid;
    seL4_CPtr      reply_slot;     /* SaveCaller'd in taskman's CSpace */
    unsigned long  expiry_ticks;   /* rdtime tick count when nanosleep wakes */
} tm_sleeper_t;

static tm_sleeper_t g_sleepers[TM_MAX_SLEEPERS];

/* Read RISC-V's `time` CSR directly.  The kernel doesn't expose it,
 * but every hart's U-mode is allowed `rdtime` per the RISC-V spec. */
static inline unsigned long rdtime(void)
{
    unsigned long t;
    __asm__ volatile ("rdtime %0" : "=r"(t));
    return t;
}

/* qsoe_time_freq_hz comes from libqsoe (set at startup).  Same
 * conversion as in libqsoe/src/time.c — split mul to avoid overflow
 * without pulling in libgcc's __udivti3. */
static inline unsigned long ns_to_ticks(unsigned long ns)
{
    if (qsoe_time_freq_hz == 0) return 0;
    unsigned long whole = (ns / 1000000000UL) * qsoe_time_freq_hz;
    unsigned long part  = ((ns % 1000000000UL) * qsoe_time_freq_hz)
                          / 1000000000UL;
    return whole + part;
}

static inline unsigned long us_to_ticks(unsigned long us)
{
    if (qsoe_time_freq_hz == 0) return 0;
    unsigned long whole = (us / 1000000UL) * qsoe_time_freq_hz;
    unsigned long part  = ((us % 1000000UL) * qsoe_time_freq_hz)
                          / 1000000UL;
    return whole + part;
}

/* Tick-comparison helper that handles 64-bit wrap defensively
 * (rdtime won't wrap for centuries at 10 MHz, but the type lets us
 * use signed subtraction for an "is expiry past now" check). */
static inline int expired(unsigned long now, unsigned long expiry)
{
    return (long)(now - expiry) >= 0;
}

/* ----------- handlers ----------- */

int tm_nanosleep(pid_t caller_pid, unsigned long total_ns, int *out_parked)
{
    *out_parked = 0;
    /* Zero or negative → return immediately, no sleep. */
    if (total_ns == 0) return 0;

    unsigned long now    = rdtime();
    unsigned long expiry = now + ns_to_ticks(total_ns);

    for (int i = 0; i < TM_MAX_SLEEPERS; ++i) {
        if (g_sleepers[i].in_use) continue;

        seL4_CPtr slot = taskman_alloc_empty_slot();
        if (qsoe_cnode_save_caller(s_cnode_root, slot,
                                    TM_DEPTH_TASKMAN) != 0) {
            taskman_free_slot(slot);
            return -ENOMEM;
        }
        g_sleepers[i].in_use       = 1;
        g_sleepers[i].caller_pid   = caller_pid;
        g_sleepers[i].reply_slot   = slot;
        g_sleepers[i].expiry_ticks = expiry;
        *out_parked = 1;
        return 0;
    }
    return -EAGAIN;   /* sleeper table full — caller can retry */
}

int tm_setitimer(pid_t caller_pid, int which,
                 unsigned long value_us, unsigned long interval_us,
                 unsigned long *out_old_value_us,
                 unsigned long *out_old_interval_us)
{
    /* v0.7 supports only ITIMER_REAL (0).  VIRTUAL/PROF need per-
     * process CPU-time accounting, deferred. */
    if (which != 0) return -EINVAL;

    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;

    unsigned long now = rdtime();

    /* Hand back the previous value in microseconds.  If a timer was
     * armed, compute remaining ticks → microseconds. */
    if (out_old_value_us) {
        if (p->itimer_expiry_ticks && !expired(now, p->itimer_expiry_ticks)) {
            unsigned long rem = p->itimer_expiry_ticks - now;
            /* ticks → us: us = ticks * 1e6 / freq */
            unsigned long whole = (rem / qsoe_time_freq_hz) * 1000000UL;
            unsigned long part  = ((rem % qsoe_time_freq_hz) * 1000000UL)
                                  / qsoe_time_freq_hz;
            *out_old_value_us = whole + part;
        } else {
            *out_old_value_us = 0;
        }
    }
    if (out_old_interval_us) {
        if (p->itimer_interval_ticks) {
            unsigned long iv = p->itimer_interval_ticks;
            unsigned long whole = (iv / qsoe_time_freq_hz) * 1000000UL;
            unsigned long part  = ((iv % qsoe_time_freq_hz) * 1000000UL)
                                  / qsoe_time_freq_hz;
            *out_old_interval_us = whole + part;
        } else {
            *out_old_interval_us = 0;
        }
    }

    if (value_us == 0) {
        /* Disarm. */
        p->itimer_expiry_ticks   = 0;
        p->itimer_interval_ticks = 0;
    } else {
        p->itimer_expiry_ticks   = now + us_to_ticks(value_us);
        p->itimer_interval_ticks = us_to_ticks(interval_us);
    }
    return 0;
}

/* ----------- the sweep, called from main.c's dispatch loop ----------- */

extern int tm_pulse_send_from_taskman(pid_t target_pid, int target_chid,
                                       int code, int value);  /* see below */

/* Tiny helper: deliver a SIGALRM-style pulse to the given process's
 * signal channel.  We can't call tm_pulse_send (that's the wire-op
 * helper that expects a connection-slot argument and uses caller's
 * scoid lookups); instead we walk taskman's channel table directly
 * to find (pid, signal_chid) and signal its bound Notification.
 *
 * No-op on processes that haven't registered a signal_chid yet
 * (signal-thread infrastructure not up); the timer still re-arms. */
static void deliver_alarm_to(pid_t target_pid, int target_chid)
{
    if (target_chid == 0) return;
    int chidx = tm_channel_index(target_pid, target_chid);
    if (chidx < 0) return;
    tm_channel_t *ch = &tm_channels_array()[chidx];
    if (!ch->in_use) return;
    /* Push a pulse record into the channel's queue.  SIGALRM = 14
     * by POSIX convention; the value field carries (sender_pid). */
    if (ch->pulse_count < TM_PULSE_QUEUE_LEN) {
        int slot = ch->pulse_tail;
        ch->pulse_queue[slot].sender_pid = QSOE_PID_TASKMAN;
        ch->pulse_queue[slot].priority   = 0;
        ch->pulse_queue[slot].code       = -40;  /* _PULSE_CODE_SIGNAL */
        ch->pulse_queue[slot].value      = 14 /*SIGALRM*/
                                          | (QSOE_PID_TASKMAN << 16);
        ch->pulse_tail = (slot + 1) % TM_PULSE_QUEUE_LEN;
        ch->pulse_count++;
    }
    /* Wake the receiver if it has a bound Notification. */
    if (ch->ntfn_sig) qsoe_sys_signal(ch->ntfn_sig);
}

void tm_timer_sweep(void)
{
    unsigned long now = rdtime();

    /* 1. Nanosleep wakers. */
    for (int i = 0; i < TM_MAX_SLEEPERS; ++i) {
        if (!g_sleepers[i].in_use) continue;
        if (!expired(now, g_sleepers[i].expiry_ticks)) continue;

        /* Reply: label=0 (success), no payload. */
        seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, 0);
        qsoe_sys_send(g_sleepers[i].reply_slot, tag, 0, 0, 0, 0);
        taskman_free_slot(g_sleepers[i].reply_slot);
        g_sleepers[i].in_use = 0;
    }

    /* 2. ITIMER_REAL expiries.  Iterate processes via the public
     *    lookup-by-pid: pid range 1..TM_MAX_PROCESSES; only the
     *    in-use ones return non-NULL. */
    for (int pid_i = 1; pid_i <= TM_MAX_PROCESSES; ++pid_i) {
        tm_process_t *p = tm_process_lookup((pid_t)pid_i);
        if (!p) continue;
        if (p->itimer_expiry_ticks == 0) continue;
        if (!expired(now, p->itimer_expiry_ticks)) continue;

        deliver_alarm_to(p->pid, p->signal_chid);
        if (p->itimer_interval_ticks > 0) {
            p->itimer_expiry_ticks = now + p->itimer_interval_ticks;
        } else {
            p->itimer_expiry_ticks = 0;
        }
    }
}
