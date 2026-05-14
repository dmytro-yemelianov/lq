/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"
#include "qsh_hash.h"

Trap sigtraps[qsh_NSIG + 1];

/* +++ signals +++ */

static const struct qsh_sigpair {
    const char *const name;
    int nr;
} qsh_sigpairs[] = {
#include "signames.inc"
    {NULL, 0}};

void
inittraps(void)
{
    int i;
    const char *cs;
    const struct qsh_sigpair *pair;

    trap_exstat = -1;

    /* populate sigtraps based on sys_signame and sys_siglist */
    for (i = 1; i < qsh_NSIG; i++) {
        sigtraps[i].signal = i;
        pair = qsh_sigpairs;
        while ((pair->nr != i) && (pair->name != NULL))
            ++pair;
        cs = pair->name;
        if ((cs == NULL) || (cs[0] == '\0'))
            sigtraps[i].name = null;
        else {
            char *s;

            /* this is not optimal, what about SIGSIG1? */
            if (isCh(cs[0], 'S', 's') && isCh(cs[1], 'I', 'i') && isCh(cs[2], 'G', 'g') &&
                cs[3] != '\0') {
                /* skip leading "SIG" */
                cs += 3;
            }
            strdupx(s, cs, APERM);
            sigtraps[i].name = s;
            while ((*s = qsh_toupper(*s)))
                ++s;
            /* check for reserved names */
            if (!strcmp(sigtraps[i].name, "EXIT") || !strcmp(sigtraps[i].name, "ERR")) {
                kwarnf0(KWF_INTERNAL | KWF_WARNING | KWF_NOERRNO, Tinvname, sigtraps[i].name,
                        "signal");
                sigtraps[i].name = null;
            }
        }
        if (sigtraps[i].name == null)
            sigtraps[i].name = shf_smprintf(Tf_d, i);
        sigtraps[i].mess = qsh_sigmess(i);
        if (qsh_sigmessf(sigtraps[i].mess))
            sigtraps[i].mess = shf_smprintf(Tf_sd, "Signal", i);
    }
    sigtraps[qsh_SIGEXIT].signal = qsh_SIGEXIT;
    sigtraps[qsh_SIGEXIT].name = "EXIT";
    sigtraps[qsh_SIGEXIT].mess = "Exit trap";
    sigtraps[qsh_SIGERR].signal = qsh_SIGERR;
    sigtraps[qsh_SIGERR].name = "ERR";
    sigtraps[qsh_SIGERR].mess = "Error handler";

    sigtraps[SIGINT].flags |= TF_DFL_INTR | TF_TTY_INTR;
    sigtraps[SIGQUIT].flags |= TF_DFL_INTR | TF_TTY_INTR;
    /* SIGTERM is not fatal for interactive */
    sigtraps[SIGTERM].flags |= TF_DFL_INTR;
    sigtraps[SIGHUP].flags |= TF_FATAL;
    sigtraps[SIGCHLD].flags |= TF_SHELL_USES;

    /* these are always caught so we can clean up any temporary files. */
    setsig(&sigtraps[SIGINT], trapsig, SS_RESTORE_ORIG);
    setsig(&sigtraps[SIGQUIT], trapsig, SS_RESTORE_ORIG);
    setsig(&sigtraps[SIGTERM], trapsig, SS_RESTORE_ORIG);
    setsig(&sigtraps[SIGHUP], trapsig, SS_RESTORE_ORIG);
}

static void alarm_catcher(int sig);

void
alarm_init(void)
{
    sigtraps[SIGALRM].flags |= TF_SHELL_USES;
    setsig(&sigtraps[SIGALRM], alarm_catcher, SS_RESTORE_ORIG | SS_FORCE | SS_SHTRAP);
}

/* ARGSUSED */
static void
alarm_catcher(int sig QSH_A_UNUSED)
{
    /* this runs inside interrupt context, with errno saved */

    if (qsh_tmout_state == TMOUT_READING) {
        int left = alarm(0);

        if (left == 0) {
            qsh_tmout_state = TMOUT_LEAVING;
            intrsig = 1;
        } else
            alarm(left);
    }
}

Trap *
gettrap(const char *cs, bool igncase, bool allsigs)
{
    int i;
    Trap *p;
    char *as;

    /* signal number (1..qsh_NSIG) or 0? */

    if (ctype(*cs, C_DIGIT))
        return ((getn(cs, &i) && 0 <= i && i < qsh_NSIG) ? (&sigtraps[i]) : NULL);

    /* do a lookup by name then */

    /* this breaks SIGSIG1, but we do that above anyway */
    if (isCh(cs[0], 'S', 's') && isCh(cs[1], 'I', 'i') && isCh(cs[2], 'G', 'g') && cs[3] != '\0') {
        /* skip leading "SIG" */
        cs += 3;
    }
    if (igncase) {
        char *s;

        strdupx(as, cs, ATEMP);
        cs = s = as;
        while ((*s = qsh_toupper(*s)))
            ++s;
    } else
        as = NULL;

    /* this is idiotic, we really want a hashtable here */

    p = sigtraps;
    i = qsh_NSIG + 1;
    do {
        if (!strcmp(p->name, cs))
            goto found;
        ++p;
    } while (--i);
    goto notfound;

found:
    if (!allsigs) {
        if (p->signal == qsh_SIGEXIT || p->signal == qsh_SIGERR) {
        notfound:
            p = NULL;
        }
    }
    afree(as, ATEMP);
    return (p);
}

static k32
traphash(int signo, int extra)
{
    register k32 h;
    static volatile k32 state;
    k32 o;
    struct {
        struct timeval tv;
        void *sp;
        int i;
        int j;
    } z;

    memset(&z, 0, sizeof(z));
    qsh_TIME(z.tv);
    z.sp = &z;
    z.i = signo;
    z.j = extra;

    o = state;
    h = o ? o : (k32)1U;
    BAFHUpdateMem(h, &z, sizeof(z));
    while (state != o) {
        o = state;
        BAFHUpdateMem(h, &o, sizeof(o));
    }
    state = h;
    return (h);
}

/*
 * trap signal handler
 */
void
trapsig(int i)
{
    Trap *p;
    int eno;

    eno = errno;
    traphash(i, eno);
    p = &sigtraps[i];
    trap = p->set = 1;
    if (p->flags & TF_DFL_INTR)
        intrsig = 1;
    if ((p->flags & TF_FATAL) && !p->trap) {
        fatal_trap = 1;
        intrsig = 1;
    }
    if (p->shtrap)
        (*p->shtrap)(i);
    errno = eno;
}

/*
 * called when we want to allow the user to ^C out of something - won't
 * work if user has trapped SIGINT.
 */
void
intrcheck(void)
{
    if (intrsig)
        runtraps(TF_DFL_INTR | TF_FATAL);
}

/*
 * called after EINTR to check if a signal with normally causes process
 * termination has been received.
 */
int
fatal_trap_check(void)
{
    Trap *p = sigtraps;
    int i = qsh_NSIG + 1;

    /* todo: should check if signal is fatal, not the TF_DFL_INTR flag */
    do {
        if (p->set && (p->flags & (TF_DFL_INTR | TF_FATAL)))
            /* return value is used as an exit code */
            return (qsh_sigmask(p->signal));
        ++p;
    } while (--i);
    return (0);
}

/*
 * Returns the signal number of any pending traps: ie, a signal which has
 * occurred for which a trap has been set or for which the TF_DFL_INTR flag
 * is set.
 */
int
trap_pending(void)
{
    Trap *p = sigtraps;
    int i = qsh_NSIG + 1;

    do {
        if (p->set &&
            ((p->trap && p->trap[0]) || ((p->flags & (TF_DFL_INTR | TF_FATAL)) && !p->trap)))
            return (p->signal);
        ++p;
    } while (--i);
    return (0);
}

/*
 * run any pending traps. If intr is set, only run traps that
 * can interrupt commands.
 */
void
runtraps(int flag)
{
    Trap *p = sigtraps;
    int i = qsh_NSIG + 1;
    k32 h;

    h = traphash(-666, (int)qsh_tmout_state);
    rndpush(&h, sizeof(h));

    if (qsh_tmout_state == TMOUT_LEAVING) {
        qsh_tmout_state = TMOUT_EXECUTING;
        kwarnf(KWF_PREFIX | KWF_ONEMSG | KWF_NOERRNO, "timed out waiting for input");
        unwind(LEXIT);
    } else
        /*
         * XXX: this means the alarm will have no effect if a trap
         * is caught after the alarm() was started...not good.
         */
        qsh_tmout_state = TMOUT_EXECUTING;
    if (!flag)
        trap = 0;
    if (flag & TF_DFL_INTR)
        intrsig = 0;
    if (flag & TF_FATAL)
        fatal_trap = 0;
    ++trap_nested;
    do {
        if (p->set && (!flag || ((p->flags & flag) && p->trap == NULL)))
            runtrap(p, false);
        ++p;
    } while (--i);
    if (!--trap_nested)
        runtrap(NULL, true);
}

void
runtrap(Trap *p, bool is_last)
{
    int old_changed = 0, i;
    char *trapstr;

    if (p == NULL)
        /* just clean up, see runtraps() above */
        goto donetrap;
    i = p->signal;
    trapstr = p->trap;
    p->set = 0;
    if (trapstr == NULL) {
        /* SIG_DFL */
        if (p->flags & (TF_FATAL | TF_DFL_INTR)) {
            exstat = (int)(128U + (unsigned)i);
            if ((unsigned)exstat > 255U)
                exstat = 255;
        }
        /* e.g. SIGHUP */
        if (p->flags & TF_FATAL)
            unwind(LLEAVE);
        /* e.g. SIGINT, SIGQUIT, SIGTERM, etc. */
        if (p->flags & TF_DFL_INTR)
            unwind(LINTR);
        goto donetrap;
    }
    if (trapstr[0] == '\0')
        /* SIG_IGN */
        goto donetrap;
    if (i == qsh_SIGEXIT || i == qsh_SIGERR) {
        /* avoid recursion on these */
        old_changed = p->flags & TF_CHANGED;
        p->flags &= ~TF_CHANGED;
        p->trap = NULL;
    }
    if (trap_exstat == -1)
        trap_exstat = exstat & 0xFF;
    /*
     * Note: trapstr is fully parsed before anything is executed, thus
     * no problem with afree(p->trap) in settrap() while still in use.
     */
    command(trapstr, current_lineno);
    if (i == qsh_SIGEXIT || i == qsh_SIGERR) {
        if (p->flags & TF_CHANGED)
            /* don't clear TF_CHANGED */
            afree(trapstr, APERM);
        else
            p->trap = trapstr;
        p->flags |= old_changed;
    }

donetrap:
    /* we're the last trap of a sequence executed */
    if (is_last && trap_exstat != -1) {
        exstat = trap_exstat;
        trap_exstat = -1;
    }
}

/* clear pending traps and reset user's trap handlers; used after fork(2) */
void
cleartraps(void)
{
    Trap *p = sigtraps;
    int i = qsh_NSIG + 1;

    trap = 0;
    intrsig = 0;
    fatal_trap = 0;

    do {
        p->set = 0;
        if ((p->flags & TF_USER_SET) && (p->trap && p->trap[0]))
            settrap(p, NULL);
        ++p;
    } while (--i);
}

/* restore signals just before an exec(2) */
void
restoresigs(void)
{
    Trap *p = sigtraps;
    int i = qsh_NSIG + 1;

    do {
        if (p->flags & (TF_EXEC_IGN | TF_EXEC_DFL))
            setsig(p, (p->flags & TF_EXEC_IGN) ? SIG_IGN : SIG_DFL, SS_RESTORE_CURR | SS_FORCE);
        ++p;
    } while (--i);
}

void
settrap(Trap *p, const char *s)
{
    sig_t f;

    afree(p->trap, APERM);
    /* handles s == NULL */
    strdupx(p->trap, s, APERM);
    p->flags |= TF_CHANGED;
    f = !s ? SIG_DFL : s[0] ? trapsig : SIG_IGN;

    p->flags |= TF_USER_SET;
    if ((p->flags & (TF_DFL_INTR | TF_FATAL)) && f == SIG_DFL)
        f = trapsig;
    else if (p->flags & TF_SHELL_USES) {
        if (!(p->flags & TF_ORIG_IGN) || Flag(FTALKING)) {
            /* do what user wants at exec time */
            p->flags &= ~(TF_EXEC_IGN | TF_EXEC_DFL);
            if (f == SIG_IGN)
                p->flags |= TF_EXEC_IGN;
            else
                p->flags |= TF_EXEC_DFL;
        }

        /*
         * assumes handler already set to what shell wants it
         * (normally trapsig, but could be j_sigchld() or SIG_IGN)
         */
        return;
    }

    /* todo: should we let user know signal is ignored? how? */
    setsig(p, f, SS_RESTORE_CURR | SS_USER);
}

/*
 * called by c_print() when writing to a co-process to ensure
 * SIGPIPE won't kill shell (unless user catches it and exits)
 */
bool
block_pipe(void)
{
    bool restore_dfl = false;
    Trap *p = &sigtraps[SIGPIPE];

    if (!(p->flags & (TF_ORIG_IGN | TF_ORIG_DFL))) {
        setsig(p, SIG_IGN, SS_RESTORE_CURR);
        if (p->flags & TF_ORIG_DFL)
            restore_dfl = true;
    } else if (p->cursig == SIG_DFL) {
        setsig(p, SIG_IGN, SS_RESTORE_CURR);
        /* restore to SIG_DFL */
        restore_dfl = true;
    }
    return (restore_dfl);
}

/* called by c_print() to undo whatever block_pipe() did */
void
restore_pipe(void)
{
    setsig(&sigtraps[SIGPIPE], SIG_DFL, SS_RESTORE_CURR);
}

/*
 * Set action for a signal. Action may not be set if original
 * action was SIG_IGN, depending on the value of flags and FTALKING.
 */
int
setsig(Trap *p, sig_t f, int flags)
{
    if (p->signal == qsh_SIGEXIT || p->signal == qsh_SIGERR)
        return (1);

    /*
     * First time setting this signal? If so, get and note the current
     * setting.
     */
    if (!(p->flags & (TF_ORIG_IGN | TF_ORIG_DFL))) {
        qsh_sigsaved ohandler;

        qsh_sigset(p->signal, SIG_IGN, &ohandler);
        p->flags |= qsh_sighandler(ohandler) == SIG_IGN ? TF_ORIG_IGN : TF_ORIG_DFL;
        p->cursig = SIG_IGN;
    }

    /*-
     * Generally, an ignored signal stays ignored, except if
     *  - the user of an interactive shell wants to change it
     *  - the shell wants for force a change
     */
    if ((p->flags & TF_ORIG_IGN) && !(flags & SS_FORCE) && (!(flags & SS_USER) || !Flag(FTALKING)))
        return (0);

    setexecsig(p, flags & SS_RESTORE_MASK);

    /*
     * This is here 'cause there should be a way of clearing
     * shtraps, but don't know if this is a sane way of doing
     * it. At the moment, all users of shtrap are lifetime
     * users (SIGALRM, SIGCHLD, SIGWINCH).
     */
    if (!(flags & SS_USER))
        p->shtrap = (sig_t)NULL;
    if (flags & SS_SHTRAP) {
        p->shtrap = f;
        f = trapsig;
    }

    if (p->cursig != f) {
        p->cursig = f;
        qsh_sigset(p->signal, f, NULL);
    }

    return (1);
}

/* control what signal is set to before an exec() */
void
setexecsig(Trap *p, int restore)
{
    /* XXX debugging */
    if (!(p->flags & (TF_ORIG_IGN | TF_ORIG_DFL)))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, "setexecsig: unset signal %d(%s)",
               p->signal, p->name);

    /* restore original value for exec'd kids */
    p->flags &= ~(TF_EXEC_IGN | TF_EXEC_DFL);
    switch (restore & SS_RESTORE_MASK) {
    case SS_RESTORE_CURR:
        /* leave things as they currently are */
        break;
    case SS_RESTORE_ORIG:
        p->flags |= p->flags & TF_ORIG_IGN ? TF_EXEC_IGN : TF_EXEC_DFL;
        break;
    case SS_RESTORE_DFL:
        p->flags |= TF_EXEC_DFL;
        break;
    case SS_RESTORE_IGN:
        p->flags |= TF_EXEC_IGN;
        break;
    }
}

#if HAVE_PERSISTENT_HISTORY || defined(DF)
/*
 * File descriptor locking and unlocking functions.
 * Could use some error handling, but hey, this is only
 * advisory locking anyway, will often not work over NFS,
 * and you are SOL if this fails...
 */

void
qsh_lockfd(int fd)
{
    int rv;
    struct flock lks;

    memset(&lks, 0, sizeof(lks));
    lks.l_type = F_WRLCK;
    do {
        rv = fcntl(fd, F_SETLKW, &lks);
    } while (rv == 1 && errno == EINTR);
}

/* designed to not define qsh_unlkfd if none triggered */
void
qsh_unlkfd(int fd)
{
    struct flock lks;

    memset(&lks, 0, sizeof(lks));
    lks.l_type = F_UNLCK;
    fcntl(fd, F_SETLKW, &lks);
}
#endif

/*
 * On handling errors when setting signals
 *
 * The signal management calls fail with:
 * - EFAULT: (sigaction only) &sa or old point to invalid memory,
 *   which is not bloody likely
 * - E?????: (sigaction only) if SA_SIGINFO is set and […]
 *   but we don’t ever set SA_SIGINFO
 * - EINVAL: on invalid signal number; operating on uncatchable
 *   respectively unignorable signals (SIGKILL, SIGSTOP)
 *
 * The signal number is already verified in higher-up code. We
 * deliberately do not error nor even warn for SIGKILL or SIGSTOP
 * because startx on Debian for example attempts that (POSIX says
 * undefined); therefore silently ignoring invalid signal numbers
 * that otherwise pass muster is acceptable.
 */

#ifndef SIG_ERR
static void
qsh_sigerr(int sig QSH_A_UNUSED)
{
}
#define SIG_ERR (&qsh_sigerr)
#endif

/* masks the signal, does not (may) restart, not oneshot */
void
qsh_sigset(int sig, sig_t act, qsh_sigsaved *old)
{
    int rv;

    if (act != SIG_ERR) {
        struct sigaction sa;

        memset(&sa, '\0', sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = act;
        rv = sigaction(sig, &sa, old);
    } else if (!old)
        return;
    else
        rv = sigaction(sig, NULL, old);
    if (rv && old) {
        memset(old, '\0', sizeof(*old));
        old->sa_handler = SIG_ERR;
    }
}

void
qsh_sigrestore(int sig, qsh_sigsaved *savedp)
{
    if (savedp->sa_handler != SIG_ERR)
        sigaction(sig, savedp, NULL);
}
