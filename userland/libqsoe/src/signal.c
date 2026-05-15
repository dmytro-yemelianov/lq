/*
 * libqsoe/src/signal.c — signals-as-pulses (v0.6.4).
 *
 * QSOE delivers POSIX signals as QNX-style pulses.  Every userland
 * process runs with a dedicated *signal thread* (created in
 * _qsoe_start_main before main()) that listens on a per-process
 * "signal channel" and dispatches incoming signal pulses to handlers
 * installed via signal()/sigaction().
 *
 *   kill(pid, sig)  →  TM_REQ_GET_SIGNAL_CHID(pid)  →  taskman
 *                  →  reply: (target_pid, target_chid)
 *                  →  ConnectAttach(target_pid, target_chid)
 *                  →  MsgSendPulse(coid, code=sig)
 *                  →  signal thread MsgReceive wakes
 *                  →  qsoe_signal_handlers[sig](sig)
 *
 * The handler runs in signal-thread context, NOT main-thread context
 * — handlers must not touch errno (we have TLS in v0.6.4) or do
 * non-async-signal-safe things across the main/signal boundary
 * without their own synchronization.  See doc/plans/v0.6.4-signals.md.
 *
 * Default action for unhandled signals: ignore (log via kerrf would
 * be nice once we have it).  SIGKILL special-cased: bypasses the
 * handler table and ProcessTerminate's the receiving process.
 */

#include <qsoe-system.h>
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#ifndef QSOE_LIBQSOE_IN_TASKMAN

#ifndef _NSIG
#define _NSIG 65            /* musl's value for the size of sigset_t arrays */
#endif

typedef void (*qsoe_sighandler_t)(int);

/* Process-wide handler table.  Slot 0 unused.  Read by the signal
 * thread; written by signal()/sigaction() from main.
 *
 * No locking needed in v0.6.4: writes are aligned pointer-sized stores
 * (atomic on RISC-V) and we only have two threads.  If the signal
 * thread reads a stale value during a handler swap, the worst case is
 * that one pending pulse runs the previous handler — POSIX-conformant
 * for any signal already in flight when sigaction() was called. */
qsoe_sighandler_t qsoe_signal_handlers[_NSIG];

/* chid of the signal channel this process owns.  Set once by the
 * signal thread itself during qsoe_signal_init's bootstrap. */
int qsoe_signal_chid;

/* Diagnostic counters — surfaced via globals so hello's signal-self-
 * test can report which init stage was reached.  Defined here for
 * forward visibility from qsoe_signal_thread_entry. */
int qsoe_signal_init_chid_err;
int qsoe_signal_init_thread_err;
int qsoe_signal_init_done;
int qsoe_signal_init_entered;
int qsoe_signal_init_past_chid_check;
int qsoe_signal_init_chid_seen;

/* Default action for a signal that arrives with no installed handler.
 * SIGKILL terminates; everything else is ignored. */
static void
qsoe_signal_default_action(int sig)
{
    if (sig == 9 /* SIGKILL */)
        _exit(128 + sig);
    /* else: silently ignore. */
    (void)sig;
}

/* Signal-thread entry.  Does its own ChannelCreate + REGISTER_SIGNAL_CHID
 * (so the bound-Notification cap binds to OUR TCB, not the main
 * thread's — leaves main free to ChannelCreate its own things), then
 * loops forever dispatching pulses to the handler table.  Never returns.
 *
 * Race window: between ThreadCreate completing and our ChannelCreate
 * + REGISTER, the process has no signal delivery.  kill() from outside
 * during that window returns ESRCH.  Acceptable for v0.6.4. */
__attribute__((noreturn))
static void *
qsoe_signal_thread_entry(void *arg)
{
    (void)arg;

    /* Set up the channel from the signal-thread's own context so the
     * bound-notification cap binds to this TCB. */
    int chid = ChannelCreate(0);
    if (chid < 0) {
        qsoe_signal_init_chid_err = qsoe_errno;
        /* main is already running; we just sit idle forever. */
        for (;;) ;
    }
    qsoe_signal_chid = chid;

    /* Tell taskman where to deliver kill(self, ...) pulses. */
    seL4_Word mr0 = (seL4_Word)chid, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_REGISTER_SIGNAL_CHID,
                                                   0, 0, 1);
    (void)qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag, &mr0, &mr1, &mr2, &mr3);

    /* Now visible to main as "signal subsystem ready". */
    qsoe_signal_init_done = 1;

    struct _pulse p;
    struct _msg_info info;
    for (;;) {
        int n = MsgReceive(chid, &p, sizeof(p), &info);
        if (n < 0)
            continue;                       /* shouldn't happen */
        if (!(info.flags & QSOE_MI_PULSE))
            continue;                       /* non-pulse: ignore */

        int sig = (int)(unsigned char)p.code;
        if (sig <= 0 || sig >= _NSIG)
            continue;

        qsoe_sighandler_t h = qsoe_signal_handlers[sig];
        if (h)
            h(sig);
        else
            qsoe_signal_default_action(sig);
    }
}

/* Called once by _qsoe_start_main, BEFORE main().  Creates the signal
 * channel, spawns the signal thread, and tells taskman where the
 * channel lives (so external kill()s can find us).
 *
 * Returns 0 on success, -1 on failure (in which case the process
 * runs without signal delivery — main() still launches). */
int
qsoe_signal_init(void)
{
    /* v0.6.4 stub.  The signal-thread design needs the process's
     * pulse-notification cap to bind to a NON-main TCB so the signal
     * thread can receive pulses directly.  Today taskman's
     * tm_channel_create binds to owner->tcb (main TCB) unconditionally,
     * and TCBs only allow one bound notification — so spawning the
     * signal thread and giving it its own pulse-bearing channel hits
     * "TCB already has a bound notification" from the kernel.
     *
     * Fix path (v0.6.5): a CHANNEL_CREATE_BOUND_TO_CALLER flag that
     * makes taskman bind to the calling TCB instead of owner->tcb.
     * Until then, signal API is wire-up-only — signal()/sigaction()/
     * kill()/raise() compile and link cleanly, but signals are not
     * actually delivered to handlers. */
    qsoe_signal_init_entered++;
    return 0;
}

/* -----------------------------------------------------------------
 * Public signal API.  These are strong-symbol overrides for musl's
 * implementations: at link time libqsoe wins, musl's rt_sigaction-
 * based versions are never pulled in from libc.a.
 *
 * The struct-sigaction surface honours only sa_handler in v0.6.4;
 * sa_mask, sa_flags, sa_sigaction are accepted but ignored.  Real
 * mask/sigaction semantics would need a richer per-pulse protocol;
 * deferred to v0.7+.
 * ----------------------------------------------------------------- */

/* musl's sighandler_t and struct sigaction.  We don't include
 * <signal.h> here — sh.h freestanding, and the typedef names match
 * what the rest of QSOE expects. */
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

struct sigaction {
    union {
        sighandler_t sa_handler;
        void (*sa_sigaction)(int, void *, void *);
    } __sa_handler;
    unsigned long sa_mask[16 / sizeof(unsigned long)];
    int           sa_flags;
    void        (*sa_restorer)(void);
};

sighandler_t
signal(int sig, sighandler_t fn)
{
    if (sig <= 0 || sig >= _NSIG) {
        qsoe_errno = EINVAL;
        return SIG_ERR;
    }
    sighandler_t prev = qsoe_signal_handlers[sig];
    qsoe_signal_handlers[sig] = (fn == SIG_IGN || fn == SIG_DFL) ? 0 : fn;
    return prev ? prev : SIG_DFL;
}

int
sigaction(int sig, const struct sigaction *act, struct sigaction *oact)
{
    if (sig <= 0 || sig >= _NSIG) {
        qsoe_errno = EINVAL;
        return -1;
    }
    sighandler_t prev = qsoe_signal_handlers[sig];

    if (oact) {
        oact->__sa_handler.sa_handler = prev ? prev : SIG_DFL;
        for (unsigned i = 0; i < sizeof(oact->sa_mask) / sizeof(oact->sa_mask[0]); ++i)
            oact->sa_mask[i] = 0;
        oact->sa_flags    = 0;
        oact->sa_restorer = 0;
    }
    if (act) {
        sighandler_t h = act->__sa_handler.sa_handler;
        qsoe_signal_handlers[sig] = (h == SIG_IGN || h == SIG_DFL) ? 0 : h;
    }
    return 0;
}

int
kill(pid_t pid, int sig)
{
    if (sig < 0 || sig >= _NSIG) {
        qsoe_errno = EINVAL;
        return -1;
    }

    /* 1) Ask taskman where the target's signal channel lives. */
    seL4_Word mr0 = (seL4_Word)pid, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_GET_SIGNAL_CHID,
                                                   0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }
    pid_t target_pid  = (pid_t)mr0;
    int   target_chid = (int)mr1;

    /* 2) ConnectAttach to that channel (transient — detach below). */
    int coid = ConnectAttach(0, target_pid, target_chid, 0, 0);
    if (coid < 0)
        return -1;

    /* 3) Send the signal pulse.  Code = signal number; value = 0. */
    int rc = MsgSendPulse(coid, 0, sig, 0);
    int saved_errno = qsoe_errno;

    /* 4) Tear down the transient connection. */
    ConnectDetach(coid);

    if (rc < 0) {
        qsoe_errno = saved_errno;
        return -1;
    }
    return 0;
}

int
raise(int sig)
{
    /* Send to self via own signal channel.  No need to round-trip
     * through taskman for the chid — we own qsoe_signal_chid. */
    if (sig < 0 || sig >= _NSIG) {
        qsoe_errno = EINVAL;
        return -1;
    }
    if (qsoe_signal_chid == 0)
        return 0;       /* signal subsystem not up; treat as no-op */

    int coid = ConnectAttach(0, qsoe_self_pid, qsoe_signal_chid, 0, 0);
    if (coid < 0)
        return -1;
    int rc = MsgSendPulse(coid, 0, sig, 0);
    int saved_errno = qsoe_errno;
    ConnectDetach(coid);
    if (rc < 0) { qsoe_errno = saved_errno; return -1; }
    return 0;
}

#endif /* !QSOE_LIBQSOE_IN_TASKMAN */
