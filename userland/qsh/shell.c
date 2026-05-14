/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Shell run loop, environment stack, cleanup.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

static void reclaim(void);
static void remove_temps(struct temp *);

#define reclim_trace() /* nothing */

int
include(const char *name, const char **argv, bool intr_ok)
{
    Source *volatile s = NULL;
    struct shf *shf;
    const char **volatile old_argv;
    volatile int old_argc;
    int i;

    shf = shf_open(name, O_RDONLY | O_MAYEXEC, 0, SHF_MAPHI | SHF_CLEXEC);
    if (shf == NULL)
        return (-1);

    if (argv) {
        old_argv = e->loc->argv;
        old_argc = e->loc->argc;
    } else {
        old_argv = NULL;
        old_argc = 0;
    }
    newenv(E_INCL);
    if ((i = qshsetjmp(e->jbuf))) {
        quitenv(s ? s->u.shf : NULL);
        if (old_argv) {
            e->loc->argv = old_argv;
            e->loc->argc = old_argc;
        }
        switch (i) {
        case LRDERR:
            return (-2);
        case LRETURN:
        case LERROR:
        case LERREXT:
            /* see below */
            return (exstat & 0xFF);
        case LINTR:
            /*
             * intr_ok is set if we are including .profile or $ENV.
             * If user ^Cs out, we don't want to kill the shell...
             */
            if (intr_ok && ((exstat & 0xFF) - 128) != SIGTERM)
                return (1);
            /* FALLTHROUGH */
        case LEXIT:
        case LLEAVE:
        case LSHELL:
            unwind(i);
            /* NOTREACHED */
        default:
            kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tunexpected_type, Tunwind, Tsource,
                   i);
            /* NOTREACHED */
        }
    }
    if (argv) {
        e->loc->argv = argv;
        e->loc->argc = 0;
        while (argv[e->loc->argc + 1])
            ++e->loc->argc;
    }
    s = pushs(SFILE, ATEMP);
    s->u.shf = shf;
    strdupx(s->file, name, ATEMP);
    i = shell(s, 1);
    quitenv(s->u.shf);
    if (old_argv) {
        e->loc->argv = old_argv;
        e->loc->argc = old_argc;
    }
    /* & 0xFF to ensure value not -1 */
    return (i & 0xFF);
}

/* spawn a command into a shell optionally keeping track of the line number */
int
command(const char *comm, int line)
{
    Source *s, *sold = source;
    int rv;

    s = pushs(SSTRING, ATEMP);
    s->start = s->str = comm;
    s->line = line;
    rv = shell(s, 1);
    source = sold;
    return (rv);
}

/*
 * run the commands from the input source, returning status.
 */
int
shell(Source *volatile s, volatile int level)
{
    struct op *t;
    volatile bool wastty = ((bool)(s->flags & SF_TTY));
    volatile kby attempts = 13;
    volatile bool interactive = (level == 0) && Flag(FTALKING);
    Source *volatile old_source = source;
    int i;

    newenv(level == 2 ? E_EVAL : E_PARSE);
    if (level == 2)
        e->flags |= EF_IN_EVAL;
    if (interactive)
        really_exit = false;
    switch ((i = qshsetjmp(e->jbuf))) {
    case 0:
        break;
    case LBREAK:
    case LCONTIN:
        /* assert: interactive == false */
        source = old_source;
        quitenv(NULL);
        if (level == 2) {
            /* keep on going */
            unwind(i);
            /* NOTREACHED */
        }
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tf_cant_s, Tshell,
               i == LBREAK ? Tbreak : Tcontinue);
        /* NOTREACHED */
    case LRDERR:
        if (level == 0)
            /* run exit traps */
            unwind(LEXIT);
        /* FALLTHROUGH */
    case LINTR:
        /* we get here if SIGINT not caught or ignored */
    case LERROR:
    case LERREXT:
    case LSHELL:
        if (interactive) {
            if (i == LINTR)
                shellf(Tnl);
            /*
             * Reset any eof that was read as part of a
             * multiline command.
             */
            if (Flag(FIGNOREEOF) && s->type == SEOF && wastty)
                s->type = SSTDIN;
            /*
             * Used by exit command to get back to
             * top level shell. Kind of strange since
             * interactive is set if we are reading from
             * a tty, but to have stopped jobs, one only
             * needs FMONITOR set (not FTALKING/SF_TTY)...
             */
            /* toss any input we have so far */
            yyrecursive_pop(true);
            s->start = s->str = null;
            retrace_info = NULL;
            herep = heres;
            break;
        } else if (i == LSHELL && level == 0)
            /* run exit traps */
            unwind(LEXIT);
        /* FALLTHROUGH */
    case LEXIT:
    case LLEAVE:
    case LRETURN:
        source = old_source;
        quitenv(NULL);
        if (i == LERREXT && level == 2)
            return (exstat & 0xFF);
        /* keep on going */
        unwind(i);
        /* NOTREACHED */
    default:
        source = old_source;
        quitenv(NULL);
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tunexpected_type, Tunwind, Tshell, i);
        /* NOTREACHED */
    }
    while (/* CONSTCOND */ 1) {
        if (trap)
            runtraps(0);

        if (s->next == NULL) {
            if (Flag(FVERBOSE))
                s->flags |= SF_ECHO;
            else
                s->flags &= ~SF_ECHO;
        }
        if (interactive) {
            j_notify();
            set_prompt(PS1, s);
        }
        t = compile(s, true);
        if (interactive)
            histsave(&s->line, NULL, HIST_FLUSH, true);
        if (!t)
            goto source_no_tree;
        if (t->type == TEOF) {
            if (wastty && Flag(FIGNOREEOF) && --attempts > 0) {
                shellf("Use 'exit' to leave qsh\n");
                s->type = SSTDIN;
            } else if (wastty && !really_exit && j_stopped_running()) {
                really_exit = true;
                s->type = SSTDIN;
            } else {
                /*
                 * this for POSIX which says EXIT traps
                 * shall be taken in the environment
                 * immediately after the last command
                 * executed.
                 */
                if (level == 0)
                    unwind(LEXIT);
                break;
            }
        } else if ((s->flags & SF_MAYEXEC) && t->type == TCOM)
            t->u.evalflags |= DOTCOMEXEC;
        if (!Flag(FNOEXEC) || (s->flags & SF_TTY))
            exstat = execute(t, 0, NULL) & 0xFF;

        if (t->type != TEOF && interactive && really_exit)
            really_exit = false;

    source_no_tree:
        reclaim();
    }
    source = old_source;
    quitenv(NULL);
    return (exstat & 0xFF);
}

/* return to closest error handler or shell(), exit if none found */
/* note: i MUST NOT be 0 */
void
unwind(int i)
{
    /* during eval, skip FERREXIT trap */
    if (i == LERREXT && (e->flags & EF_IN_EVAL))
        goto defer_traps;

    /* ordering for EXIT vs ERR is a bit odd (this is what AT&T ksh does) */
    if (i == LEXIT || ((i == LERROR || i == LERREXT || i == LINTR) && sigtraps[qsh_SIGEXIT].trap &&
                       (!Flag(FTALKING) || Flag(FERREXIT)))) {
        ++trap_nested;
        runtrap(&sigtraps[qsh_SIGEXIT], trap_nested == 1);
        --trap_nested;
        i = LLEAVE;
    } else if (Flag(FERREXIT) && (i == LERROR || i == LERREXT || i == LINTR)) {
        ++trap_nested;
        runtrap(&sigtraps[qsh_SIGERR], trap_nested == 1);
        --trap_nested;
        i = LLEAVE;
    }
defer_traps:

    while (/* CONSTCOND */ 1) {
        switch (e->type) {
        case E_PARSE:
        case E_FUNC:
        case E_INCL:
        case E_LOOP:
        case E_ERRH:
        case E_EVAL:
            qshlongjmp(e->jbuf, i);
            /* NOTREACHED */
        case E_NONE:
            if (i == LINTR)
                e->flags |= EF_FAKE_SIGDIE;
            /* FALLTHROUGH */
        default:
            quitenv(NULL);
        }
    }
}

void
newenv(int type)
{
    struct env *ep;
    char *cp;

    reclim_trace();
    /*
     * struct env includes ALLOC_ITEM for alignment constraints
     * so first get the actually used memory, then assign it
     */
    cp = alloc(sizeof(struct env) - sizeof(ALLOC_ITEM), ATEMP);
    /* undo what alloc() did to the malloc result address */
    ep = (void *)(cp - sizeof(ALLOC_ITEM));
    /* initialise public members of struct env (not the ALLOC_ITEM) */
    ainit(&ep->area);
    ep->oenv = e;
    ep->loc = e->loc;
    ep->savedfd = NULL;
    ep->temps = NULL;
    ep->yyrecursive_statep = NULL;
    ep->type = type;
    ep->flags = e->flags & EF_IN_EVAL;
    e = ep;
}

void
quitenv(struct shf *shf)
{
    struct env *ep = e;
    char *cp;
    int fd, i;

    yyrecursive_pop(true);
    while (ep->oenv && ep->oenv->loc != ep->loc)
        popblock();
    if (ep->savedfd != NULL) {
        for (fd = 0; fd < NUFILE; fd++)
            if ((i = SAVEDFD(ep, fd)))
                restfd(fd, i);
        if (SAVEDFD(ep, 2))
            /* Clear any write errors */
            shf_reopen(2, SHF_WR, shl_out);
    }
    if (ep->type == E_EXEC) {
        /* could be set, would be reclaim()ed below */
        builtin_argv0 = NULL;
    }
    /*
     * Bottom of the stack.
     * Either main shell is exiting or cleanup_parents_env() was called.
     */
    if (ep->oenv == NULL) {
        if (ep->type == E_NONE) {
            /* Main shell exiting? */
            j_exit();
            if (ep->flags & EF_FAKE_SIGDIE) {
                int sig = (exstat & 0xFF) - 128;

                /*
                 * ham up our death a bit (AT&T ksh
                 * only seems to do this for SIGTERM)
                 * Don't do it for SIGQUIT, since we'd
                 * dump a core..
                 */
                if ((sig == SIGINT || sig == SIGTERM) && (qshpgrp == qshpid)) {
                    setsig(&sigtraps[sig], SIG_DFL, SS_RESTORE_CURR | SS_FORCE);
                    kill(0, sig);
                }
            }
        }
        if (shf)
            shf_close(shf);
        reclaim();
        exit(exstat & 0xFF);
    }
    if (shf)
        shf_close(shf);
    reclaim();

    e = e->oenv;

    /* free the struct env - tricky due to the ALLOC_ITEM inside */
    cp = (void *)ep;
    afree(cp + sizeof(ALLOC_ITEM), ATEMP);
}

/* Called after a fork to cleanup stuff left over from parents environment */
void
cleanup_parents_env(void)
{
    struct env *ep;
    int fd;

    /*
     * Don't clean up temporary files - parent will probably need them.
     * Also, can't easily reclaim memory since variables, etc. could be
     * anywhere.
     */

    /* close all file descriptors hiding in savedfd */
    for (ep = e; ep; ep = ep->oenv) {
        if (ep->savedfd) {
            for (fd = 0; fd < NUFILE; fd++)
                if (FDSVNUM(ep, fd) > (kui)FDBASE)
                    close((int)FDSVNUM(ep, fd));
            afree(ep->savedfd, &ep->area);
            ep->savedfd = NULL;
        }
    }
    e->oenv = NULL;
}

/* Called just before an execve cleanup stuff temporary files */
void
cleanup_proc_env(void)
{
    struct env *ep;

    for (ep = e; ep; ep = ep->oenv)
        remove_temps(ep->temps);
}

/* remove temp files and free ATEMP Area */
static void
reclaim(void)
{
    struct block *l;

    while ((l = e->loc) && (!e->oenv || e->oenv->loc != l)) {
        e->loc = l->next;
        afreeall(&l->area);
    }

    remove_temps(e->temps);
    e->temps = NULL;

    /*
     * if the memory backing source is reclaimed, things
     * will end up badly when a function expecting it to
     * be valid is run; a NULL pointer is easily debugged
     */
    if (source && source->areap == &e->area)
        source = NULL;
    retrace_info = NULL;
    afreeall(&e->area);
}

static void
remove_temps(struct temp *tp)
{
    while (tp) {
        if (tp->pid == procpid)
            unlink(tp->tffn);
        tp = tp->next;
    }
}

