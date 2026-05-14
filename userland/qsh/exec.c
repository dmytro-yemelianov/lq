/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

#ifndef QSH_DEFAULT_EXECSHELL
#define QSH_DEFAULT_EXECSHELL QSH_UNIXROOT "/bin/sh"
#endif

static int comexec(struct op *, struct tbl *volatile, const char **, int volatile, volatile int *);
static void scriptexec(struct op *, const char **) QSH_A_NORETURN;
int call_builtin(struct tbl *, const char **, const char *, bool);
int iosetup(struct ioword *, struct tbl *);
const char *do_selectargs(const char **, bool);
Test_op dbteste_isa(Test_env *, Test_meta);
const char *dbteste_getopnd(Test_env *, Test_op, bool);
void dbteste_error(Test_env *, int, const char *);
/* XXX: horrible kludge to fit within the framework */
static void plain_fmt_entry(char *, size_t, unsigned int, const void *);
static void select_fmt_entry(char *, size_t, unsigned int, const void *);

/*
 * execute command tree
 */
int
execute(struct op *volatile t,
        /* if XEXEC don't fork */
        volatile int flags, volatile int *volatile xerrok)
{
    int i;
    volatile int rv = 0, dummy = 0;
    int pv[2];
    const char **volatile ap = NULL;
    char **volatile up;
    const char *s, *ccp;
    struct ioword **iowp;
    struct tbl *tp = NULL;

    if (t == NULL)
        return (0);

    /* Caller doesn't care if XERROK should propagate. */
    if (xerrok == NULL)
        xerrok = &dummy;

    if (flags & XFORK)
        switch (t->type) {
        case TPIPE:
            break;
        default:
            if (flags & XEXEC)
                break;
            /* FALLTHROUGH */
        case TTIME:
            /* run in sub-process */
            return (exchild(t, flags & ~XTIME, xerrok, -1));
        }

    newenv(E_EXEC);
    if (trap)
        runtraps(0);

    /* we want to run an executable, do some variance checks */
    if (t->type == TCOM) {
        /*
         * Clear subst_exstat before argument expansion. Used by
         * null commands (see comexec() and c_eval()) and by c_set().
         */
        subst_exstat = 0;

        /* for $LINENO */
        current_lineno = t->lineno;

        /* check if this is 'var=<<EOF' */
        if (
            /* we have zero arguments, i.e. no program to run */
            t->args[0] == NULL &&
            /* we have exactly one variable assignment */
            t->vars[0] != NULL && t->vars[1] == NULL &&
            /* we have exactly one I/O redirection */
            t->ioact != NULL && t->ioact[0] != NULL && t->ioact[1] == NULL &&
            /* of type "here document" (or "here string") */
            (t->ioact[0]->ioflag & IOTYPE) == IOHERE &&
            /* the variable assignment begins with a valid varname */
            (ccp = skip_wdvarname(t->vars[0], true)) != t->vars[0] &&
            /* and has no right-hand side (i.e. "varname=") */
            ccp[0] == CHAR &&
            ((ccp[1] == '=' && ccp[2] == EOS) ||
             /* or "varname+=" */ (ccp[1] == '+' && ccp[2] == CHAR && ccp[3] == '=' &&
                                   ccp[4] == EOS))) {
            char *cp, *dp;

            if ((rv = herein(t->ioact[0], &cp) /*? 1 : 0*/))
                cp = NULL;
            strdup2x(dp, evalstr(t->vars[0], DOASNTILDE | DOSCALAR), rv ? null : cp);
            typeset(dp, Flag(FEXPORT) ? EXPORT : 0, 0, 0, 0);
            /* free the expanded value */
            afree(cp, APERM);
            afree(dp, ATEMP);
            goto Break;
        }

        /*
         * POSIX says expand command words first, then redirections,
         * and assignments last..
         */
        up = eval(t->args, t->u.evalflags | DOBLANK | DOGLOB | DOTILDE);
        if (flags & XTIME)
            /* Allow option parsing (bizarre, but POSIX) */
            timex_hook(t, &up);
        ap = (const char **)up;
        if (ap[0])
            tp = findcom(ap[0], FC_BI | FC_FUNC);
    }
    flags &= ~XTIME;

    if (t->ioact != NULL || t->type == TPIPE || t->type == TCOPROC) {
        e->savedfd = alloc2(NUFILE, sizeof(qsh_fdsave), ATEMP);
        /* initialise to not redirected */
        memset(e->savedfd, 0, NUFILE * sizeof(qsh_fdsave));
    }

    /* mark for replacement later (unless TPIPE) */
    vp_pipest->flag |= INT_L;

    /* do redirection, to be restored in quitenv() */
    if (t->ioact != NULL)
        for (iowp = t->ioact; *iowp != NULL; iowp++) {
            if (iosetup(*iowp, tp) < 0) {
                exstat = rv = 1;
                /*
                 * Redirection failures for special commands
                 * cause (non-interactive) shell to exit.
                 */
                if (tp && tp->type == CSHELL && (tp->flag & SPEC_BI))
                    kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG | KWF_NOERRNO,
                          "redirection failure");
                /* Deal with FERREXIT, quitenv(), etc. */
                goto Break;
            }
        }

    switch (t->type) {
    case TCOM:
        rv = comexec(t, tp, (const char **)ap, flags, xerrok);
        break;

    case TPAREN:
        rv = execute(t->left, flags | XFORK, xerrok);
        break;

    case TPIPE:
        flags |= XFORK;
        flags &= ~XEXEC;
        FDSAVE(0, savefd(0));
        FDSAVE(1, savefd(1));
        while (t->type == TPIPE) {
            openpipe(pv);
            /* stdout of curr */
            qsh_dup2(pv[1], 1, false);
            /**
             * Let exchild() close pv[0] in child
             * (if this isn't done, commands like
             *  (: ; cat /etc/termcap) | sleep 1
             * will hang forever).
             */
            exchild(t->left, flags | XPIPEO | XCCLOSE, NULL, pv[0]);
            /* stdin of next */
            qsh_dup2(pv[0], 0, false);
            closepipe(pv);
            flags |= XPIPEI;
            t = t->right;
        }
        /* stdout of last */
        restfd(1, SAVEDFD(e, 1));
        /* no need to re-restore this */
        e->savedfd[1] = 0;
        /* Let exchild() close 0 in parent, after fork, before wait */
        i = exchild(t, flags | XPCLOSE | XPIPEST, xerrok, 0);
        if (!(flags & XBGND) && !(flags & XXCOM))
            rv = i;
        break;

    case TLIST:
        while (t->type == TLIST) {
            execute(t->left, flags & XERROK, NULL);
            t = t->right;
        }
        rv = execute(t, flags & XERROK, xerrok);
        break;

    case TCOPROC: {
        sigset_t omask;

        /*
         * Block sigchild as we are using things changed in the
         * signal handler
         */
        sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);
        e->type = E_ERRH;
        if ((i = qshsetjmp(e->jbuf))) {
            sigprocmask(SIG_SETMASK, &omask, NULL);
            quitenv(NULL);
            unwind(i);
            /* NOTREACHED */
        }
        /* Already have a (live) co-process? */
        if (coproc.job && coproc.write >= 0)
            kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG | KWF_NOERRNO,
                  "coprocess already exists");

        /* Can we re-use the existing co-process pipe? */
        coproc_cleanup(true);

        /* do this before opening pipes, in case these fail */
        FDSAVE(0, savefd(0));
        FDSAVE(1, savefd(1));

        openpipe(pv);
        if (pv[0] != 0) {
            qsh_dup2(pv[0], 0, false);
            close(pv[0]);
        }
        coproc.write = pv[1];
        coproc.job = NULL;

        if (coproc.readw >= 0)
            qsh_dup2(coproc.readw, 1, false);
        else {
            openpipe(pv);
            coproc.read = pv[0];
            qsh_dup2(pv[1], 1, false);
            /* closed before first read */
            coproc.readw = pv[1];
            coproc.njobs = 0;
            /* create new coprocess id */
            ++coproc.id;
        }
        sigprocmask(SIG_SETMASK, &omask, NULL);
        /* no more need for error handler */
        e->type = E_EXEC;

        /*
         * exchild() closes coproc.* in child after fork,
         * will also increment coproc.njobs when the
         * job is actually created.
         */
        flags &= ~XEXEC;
        exchild(t->left, flags | XBGND | XFORK | XCOPROC | XCCLOSE, NULL, coproc.readw);
        break;
    }

    case TASYNC:
        /*
         * XXX non-optimal, I think - "(foo &)", forks for (),
         * forks again for async... parent should optimise
         * this to "foo &"...
         */
        rv = execute(t->left, (flags & ~XEXEC) | XBGND | XFORK, xerrok);
        break;

    case TOR:
    case TAND:
        rv = execute(t->left, XERROK, NULL);
        if ((rv == 0) == (t->type == TAND))
            rv = execute(t->right, flags & XERROK, xerrok);
        else {
            flags |= XERROK;
            if (xerrok)
                *xerrok = 1;
        }
        break;

    case TBANG:
        rv = !execute(t->right, XERROK, xerrok);
        flags |= XERROK;
        if (xerrok)
            *xerrok = 1;
        break;

    case TDBRACKET: {
        Test_env te;

        te.flags = TEF_DBRACKET;
        te.pos.wp = t->args;
        te.isa = dbteste_isa;
        te.getopnd = dbteste_getopnd;
        te.eval = test_eval;
        te.error = dbteste_error;

        rv = test_parse(&te);
        break;
    }

    case TFOR:
    case TSELECT: {
        volatile bool is_first = true;

        if (t->vars == NULL)
            /* “for i; do” */
            ap = cpyargv(NULL, e->loc->argv, ATEMP) + 1;
        else
            ap = (const char **)eval((const char **)t->vars, DOBLANK | DOGLOB | DOTILDE);
        e->type = E_LOOP;
        while ((i = qshsetjmp(e->jbuf))) {
            if ((e->flags & EF_BRKCONT_PASS) || (i != LBREAK && i != LCONTIN)) {
                quitenv(NULL);
                unwind(i);
            } else if (i == LBREAK) {
                rv = 0;
                goto Break;
            }
        }
        /* in case of a continue */
        rv = 0;
        if (t->type == TFOR) {
            while (*ap != NULL) {
                setstr(global(t->str), *ap++, QSH_UNWIND_ERROR);
                rv = execute(t->left, flags & XERROK, xerrok);
            }
        } else {
        do_TSELECT:
            if ((ccp = do_selectargs(ap, is_first))) {
                is_first = false;
                setstr(global(t->str), ccp, QSH_UNWIND_ERROR);
                execute(t->left, flags & XERROK, xerrok);
                goto do_TSELECT;
            }
            rv = 1;
        }
        break;
    }

    case TWHILE:
    case TUNTIL:
        e->type = E_LOOP;
        while ((i = qshsetjmp(e->jbuf))) {
            if ((e->flags & EF_BRKCONT_PASS) || (i != LBREAK && i != LCONTIN)) {
                quitenv(NULL);
                unwind(i);
            } else if (i == LBREAK) {
                rv = 0;
                goto Break;
            }
        }
        /* in case of a continue */
        rv = 0;
        while ((execute(t->left, XERROK, NULL) == 0) == (t->type == TWHILE))
            rv = execute(t->right, flags & XERROK, xerrok);
        break;

    case TIF:
    case TELIF:
        if (t->right == NULL)
            /* should be error */
            break;
        rv = execute(execute(t->left, XERROK, NULL) == 0 ? t->right->left : t->right->right,
                     flags & XERROK, xerrok);
        break;

    case TCASE:
        i = 0;
        ccp = evalstr(t->str, DOTILDE | DOSCALAR);
        for (t = t->left; t != NULL && t->type == TPAT; t = t->right) {
            for (ap = (const char **)t->vars; *ap; ap++) {
                if (i || ((s = evalstr(*ap, DOTILDE | DOPAT)) && gmatchx(ccp, s, false))) {
                    record_match(ccp);
                    rv = execute(t->left, flags & XERROK, xerrok);
                    i = 0;
                    switch (t->u.charflag) {
                    case '&':
                        i = 1;
                        /* FALLTHROUGH */
                    case '|':
                        goto TCASE_next;
                    }
                    goto TCASE_out;
                }
            }
            i = 0;
        TCASE_next:
            /* empty */;
        }
    TCASE_out:
        break;

    case TBRACE:
        rv = execute(t->left, flags & XERROK, xerrok);
        break;

    case TFUNCT:
        rv = define(t->str, t);
        break;

    case TTIME:
        /*
         * Clear XEXEC so nested execute() call doesn't exit
         * (allows "ls -l | time grep foo").
         */
        rv = timex(t, flags & ~XEXEC, xerrok);
        break;

    case TEXEC:
        /*
         * QRV: execute an external command via posix_spawn() + wait,
         * not fork+exec.  The shell stays resident; redirections
         * already applied to our fds (above, around line 132) are
         * inherited by the spawned child by default, then restored
         * for the shell by quitenv() on the way out.
         *
         * Clear FD_CLOEXEC on x>&x dup-self redirections so the
         * spawned child inherits them.
         */
        up = makenv();
        if (!Flag(FPOSIX) && !Flag(FSH) && t->left->ioact != NULL)
            for (iowp = t->left->ioact; *iowp != NULL; iowp++)
                if (((*iowp)->ioflag & IODUPSELF) && fcntl((*iowp)->unit, F_SETFD, 0) == -1)
                    kwarnf0(KWF_INTERNAL | KWF_WARNING, Tcloexec_failed, "clear", (*iowp)->unit);
        {
            union qsh_ccphack cargs;
            pid_t cpid;
            int wstatus;

            cargs.ro = t->args;
            rv = posix_spawn(&cpid, t->str, NULL, NULL, cargs.rw, up);
            if (rv == ENOEXEC) {
                /* binary exists but isn't a native ELF — try interpreter */
                scriptexec(t, (const char **)up);
                /* scriptexec doesn't return on success, falls through on err */
            }
            if (rv != 0)
                kerrf(KWF_VERRNO | KWF_ERR(126) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG,
                      rv, t->str);
            /* wait for the child to complete — returns its exit status */
            if (waitpid(cpid, &wstatus, 0) < 0)
                rv = errno;
            else if (WIFEXITED(wstatus))
                rv = WEXITSTATUS(wstatus);
            else if (WIFSIGNALED(wstatus))
                rv = qsh_sigmask(WTERMSIG(wstatus));
            else
                rv = 1;
        }
    }
Break:
    exstat = rv & 0xFF;
    if (vp_pipest->flag & INT_L) {
        unset(vp_pipest, 1);
        vp_pipest->flag = DEFINED | ISSET | INTEGER | RDONLY | ARRAY | INT_U | INT_L;
        vp_pipest->val.i = rv;
    }

    /* restores IO */
    quitenv(NULL);
    if ((flags & XEXEC))
        /* exit child */
        unwind(LEXIT);
    if (rv != 0 && !(flags & XERROK) && (xerrok == NULL || !*xerrok)) {
        trapsig(qsh_SIGERR);
        if (Flag(FERREXIT))
            unwind(LERREXT);
    }
    return (rv);
}

/*
 * execute simple command
 */

static int
comexec(struct op *t, struct tbl *volatile tp, const char **ap, volatile int flags,
        volatile int *xerrok)
{
    int i;
    volatile int rv = 0;
    const char *cp;
    const char **lastp;
    int type_flags;
    bool resetspec;
    int fcflags = FC_BI | FC_FUNC | FC_PATH;
    struct block *l_expand, *l_assign;
    int optc;
    const char *exec_argv0 = NULL;
    bool exec_clrenv = false;
    volatile kui old_inuse;
    const char *volatile old_kshname;
    volatile kby old_flags[FNFLAGS];
    static struct op texec; /* static for use by child process */

    /* snag the last argument for $_ */
    if (Flag(FTALKING) && *(lastp = ap)) {
        /*
         * XXX not the same as AT&T ksh, which only seems to set $_
         * after a newline (but not in functions/dot scripts, but in
         * interactive and script) - perhaps save last arg here and
         * set it in shell()?.
         */
        while (*++lastp)
            ;
        /* setstr() can't fail here */
        setstr(typeset("_", LOCAL, 0, INTEGER, 0), *--lastp, QSH_RETURN_ERROR);
    }

    /**
     * Deal with the shell builtins builtin, exec and command since
     * they can be followed by other commands. This must be done before
     * we know if we should create a local block which must be done
     * before we can do a path search (in case the assignments change
     * PATH).
     * Odd cases:
     *  FOO=bar exec >/dev/null     FOO is kept but not exported
     *  FOO=bar exec foobar     FOO is exported
     *  FOO=bar command exec >/dev/null FOO is neither kept nor exported
     *  FOO=bar command         FOO is neither kept nor exported
     *  PATH=... foobar         use new PATH in foobar search
     */
    resetspec = false;
    while (tp && tp->type == CSHELL) {
        /* undo effects of command */
        fcflags = FC_BI | FC_FUNC | FC_PATH;
        if (tp->val.f == c_builtin) {
            if ((cp = *++ap) == NULL || (!strcmp(cp, "--") && (cp = *++ap) == NULL)) {
                tp = NULL;
                break;
            }
            if ((tp = findcom(cp, FC_BI)) == NULL)
                kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_THREEMSG | KWF_NOERRNO, Tbuiltin,
                      cp, Tnot_found);
            if (tp->type == CSHELL && (tp->flag & LOW_BI))
                break;
            continue;
        } else if (tp->val.f == c_exec) {
            if (ap[1] == NULL)
                break;
            qsh_getopt_reset(&builtin_opt, GF_ERROR);
            while ((optc = qsh_getopt(ap, &builtin_opt, "a:c")) != -1)
                switch (optc) {
                case 'a':
                    exec_argv0 = builtin_opt.optarg;
                    break;
                case 'c':
                    exec_clrenv = true;
                    /* ensure we can actually do this */
                    resetspec = true;
                    break;
                default:
                    rv = 2;
                    goto Leave;
                }
            ap += builtin_opt.optind;
            flags |= XEXEC;
            /* POSuX demands ksh88-like behaviour here */
            if (Flag(FPOSIX))
                fcflags = FC_PATH;
        } else if (tp->val.f == c_command) {
            bool saw_p = false;

            /*
             * Ugly dealing with options in two places (here
             * and in c_command(), but such is life)
             */
            qsh_getopt_reset(&builtin_opt, 0);
            while ((optc = qsh_getopt(ap, &builtin_opt, ":p")) == 'p')
                saw_p = true;
            if (optc != -1)
                /* command -vV or something */
                break;
            /* don't look for functions */
            fcflags = FC_BI | FC_PATH;
            if (saw_p) {
                if (Flag(FRESTRICTED)) {
                    kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, "command -p",
                           "restricted");
                    rv = 1;
                    goto Leave;
                }
                fcflags |= FC_DEFPATH;
            }
            ap += builtin_opt.optind;
            /*
             * POSIX says special builtins lose their status
             * if accessed using command.
             */
            resetspec = true;
            if (!ap[0]) {
                /* ensure command with no args exits with 0 */
                subst_exstat = 0;
                break;
            }
        } else if (tp->flag & LOW_BI) {
            /* if we have any flags, do not use the builtin */
            if ((ap[1] && ap[1][0] == '-' && ap[1][1] != '\0' &&
                 /* argument, begins with -, is not - or -- */
                 (ap[1][1] != '-' || ap[1][2] != '\0')) ||
                /* always prefer the external utility */
                (tp->flag & LOWER_BI)) {
                struct tbl *ext_cmd;

                ext_cmd = findcom(tp->name, FC_FUNC | FC_PATH);
                if (ext_cmd && (ext_cmd->type == CFUNC || (ext_cmd->flag & ISSET)))
                    tp = ext_cmd;
            }
            break;
        } else if (tp->val.f == c_trap) {
            t->u.evalflags &= ~DOTCOMEXEC;
            break;
        } else
            break;
        tp = findcom(ap[0], fcflags & (FC_BI | FC_FUNC));
    }
    if (t->u.evalflags & DOTCOMEXEC)
        flags |= XEXEC;
    l_expand = e->loc;
    if (!resetspec && (!ap[0] || (tp && (tp->flag & KEEPASN))))
        type_flags = 0;
    else {
        /* create new variable/function block */
        newblock();
        /* all functions keep assignments */
        type_flags = LOCAL | LOCAL_COPY | EXPORT;
    }
    l_assign = e->loc;
    if (exec_clrenv)
        l_assign->flags |= BF_STOPENV;
    if (Flag(FEXPORT))
        type_flags |= EXPORT;
    if (Flag(FXTRACE))
        change_xtrace(2, false);
    for (i = 0; t->vars[i]; i++) {
        /* do NOT lookup in the new var/fn block just created */
        e->loc = l_expand;
        cp = evalstr(t->vars[i], DOASNTILDE | DOSCALAR);
        e->loc = l_assign;
        if (Flag(FXTRACE)) {
            const char *ccp;

            ccp = skip_varname(cp, true);
            if (*ccp == '+')
                ++ccp;
            if (*ccp == '=')
                ++ccp;
            shf_write(cp, ccp - cp, shl_xtrace);
            print_value_quoted(shl_xtrace, ccp);
            shf_putc(' ', shl_xtrace);
        }
        /* but assign in there as usual */
        typeset(cp, type_flags, 0, 0, 0);
    }

    if (Flag(FXTRACE)) {
        change_xtrace(2, false);
        if (ap[rv = 0]) {
        xtrace_ap_loop:
            print_value_quoted(shl_xtrace, ap[rv]);
            if (ap[++rv]) {
                shf_putc(' ', shl_xtrace);
                goto xtrace_ap_loop;
            }
        }
        change_xtrace(1, false);
    }

    if ((cp = *ap) == NULL) {
        rv = subst_exstat;
        goto Leave;
    } else if (!tp) {
        if (Flag(FRESTRICTED) && qsh_vdirsep(cp)) {
            kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, cp, "restricted");
            rv = 1;
            goto Leave;
        }
        tp = findcom(cp, fcflags);
    }

    switch (tp->type) {

    /* shell built-in */
    case CSHELL:
    do_call_builtin:
        if (l_expand != l_assign)
            l_assign->flags |= (tp->flag & NEXTLOC_BI);
        rv = call_builtin(tp, (const char **)ap, null, resetspec);
        break;

    /* function call */
    case CFUNC:
        if (!(tp->flag & ISSET)) {
            struct tbl *ftp;

            if (!tp->u.fpath) {
            fpath_error:
                rv = (tp->u2.errnov == ENOENT) ? 127 : 126;
                kwarnf(KWF_ERR(rv) | KWF_VERRNO | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG,
                       tp->u2.errnov, cp, "function definition file");
                break;
            }
            errno = 0;
            if (include(tp->u.fpath, NULL, false) < 0 || !(ftp = findfunc(cp, hash(cp), false)) ||
                !(ftp->flag & ISSET)) {
                rv = errno;
                if ((ftp = findcom(cp, FC_BI)) && (ftp->type == CSHELL) && (ftp->flag & LOW_BI)) {
                    tp = ftp;
                    goto do_call_builtin;
                }
                if (rv) {
                    tp->u2.errnov = rv;
                    cp = tp->u.fpath;
                    goto fpath_error;
                }
                kwarnf0(KWF_ERR(127) | KWF_PREFIX | KWF_FILELINE | KWF_NOERRNO, Tf_sD_s_s, cp,
                        "function not defined by", tp->u.fpath);
                rv = 127;
                break;
            }
            tp = ftp;
        }

        /*
         * ksh functions set $0 to function name, POSIX
         * functions leave $0 unchanged.
         */
        old_kshname = qshname;
        if (tp->flag & FKSH)
            qshname = ap[0];
        else
            ap[0] = qshname;
        e->loc->argv = ap;
        for (i = 0; *ap++ != NULL; i++)
            ;
        e->loc->argc = i - 1;
        /*
         * ksh-style functions handle getopts sanely,
         * Bourne/POSIX functions are insane...
         */
        if (tp->flag & FKSH) {
            e->loc->flags |= BF_DOGETOPTS;
            e->loc->getopts_state = user_opt;
            getopts_reset(1);
        }

        for (type_flags = 0; type_flags < FNFLAGS; ++type_flags)
            old_flags[type_flags] = shell_flags[type_flags];
        change_xtrace((Flag(FXTRACEREC) ? Flag(FXTRACE) : 0) | ((tp->flag & TRACE) ? 1 : 0), false);
        old_inuse = tp->flag & FINUSE;
        tp->flag |= FINUSE;

        e->type = E_FUNC;
        if (!(i = qshsetjmp(e->jbuf))) {
            execute(tp->val.t, flags & XERROK, NULL);
            i = LRETURN;
        }

        qshname = old_kshname;
        change_xtrace(old_flags[(int)FXTRACE], false);
        if (tp->flag & FKSH) {
            /* Korn style functions restore Flags on return */
            old_flags[(int)FXTRACE] = Flag(FXTRACE);
            /* some must not be restored / need special handling */
            for (type_flags = 0; type_flags < FNFLAGS; ++type_flags)
                    if (type_flags != FPRIVILEGED)
                    shell_flags[type_flags] = old_flags[type_flags];
        }
        tp->flag = (tp->flag & ~FINUSE) | old_inuse;

        /*
         * Were we deleted while executing? If so, free the
         * execution tree.
         */
        if ((tp->flag & (FDELETE | FINUSE)) == FDELETE) {
            if (tp->flag & ALLOC) {
                tp->flag &= ~ALLOC;
                tfree(tp->val.t, tp->areap);
            }
            tp->flag = 0;
        }
        switch (i) {
        case LRETURN:
        case LERROR:
        case LERREXT:
            rv = exstat & 0xFF;
            break;
        case LRDERR:
        case LINTR:
        case LEXIT:
        case LLEAVE:
        case LSHELL:
            quitenv(NULL);
            unwind(i);
            /* NOTREACHED */
        default:
            quitenv(NULL);
            kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tunexpected_type, Tunwind, Tfunction,
                   i);
        }
        break;

    /* executable command */
    case CEXEC:
    /* tracked alias */
    case CTALIAS:
        if (!(tp->flag & ISSET)) {
            if (tp->u2.errnov == ENOENT) {
                rv = 127;
                kwarnf(KWF_ERR(127) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, cp,
                       Tinacc_not_found);
            } else {
                rv = 126;
                kwarnf(KWF_ERR(126) | KWF_VERRNO | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG,
                       tp->u2.errnov, cp, "can't execute");
            }
            break;
        }

        /* set $_ to program's full path */
        /* setstr() can't fail here */
        setstr(typeset("_", LOCAL | EXPORT, 0, INTEGER, 0), tp->val.s, QSH_RETURN_ERROR);

        /* to fork, we set up a TEXEC node and call execute */
        texec.type = TEXEC;
        /* for vistree/dumptree */
        texec.left = t;
        texec.str = tp->val.s;
        texec.args = ap;

        /* in this case we do not fork, of course */
        if (flags & XEXEC) {
            if (exec_argv0)
                texec.args[0] = exec_argv0;
            j_exit();
        }

        rv = exchild(&texec, flags, xerrok, -1);
        break;
    }
Leave:
    if (flags & XEXEC) {
        exstat = rv & 0xFF;
        unwind(LEXIT);
    }
    return (rv);
}

static void
scriptexec(struct op *tp, const char **ap)
{
    const char *sh;
    int fd;
    unsigned char buf[68];
    union qsh_ccphack args, cap;

    sh = str_val(global(TEXECSHELL));
    if (sh && *sh)
        sh = search_path(sh, path, X_OK, NULL);
    if (!sh || !*sh)
        sh = QSH_DEFAULT_EXECSHELL;

    *tp->args-- = tp->str;

    if ((fd = binopen2(tp->str, O_RDONLY | O_MAYEXEC)) >= 0) {
        unsigned char *cp;
        unsigned short m;
        ssize_t n;

        /* read first couple of octets from file */
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        /* read error or short read? */
        if (n < 5)
            goto nomagic;
        /* terminate buffer */
        buf[n] = '\0';

        /* scan for shebang magic */
        if (ord(buf[0]) == ORD('#') && ord(buf[1]) == ORD('!'))
            n = 2;
        else
            goto noshebang;
        /* scan for newline or NUL (end of buffer) */
        cp = buf + n;
        while (!ctype(*cp, C_NL | C_NUL))
            ++cp;
        /* if the shebang line is longer than MAXINTERP, bail out */
        if (!*cp)
            kerrf0(KWF_ERR(126) | KWF_PREFIX | KWF_FILELINE | KWF_NOERRNO,
                   "%s: not executable: shebang too long", tp->str);
        /* replace newline by NUL */
        *cp = '\0';

        /* restore start of shebang position */
        cp = buf + n;
        /* skip whitespace before shell name */
        while (ctype(*cp, C_BLANK))
            ++cp;
        /* just whitespace on the line? */
        if (*cp == '\0')
            goto noshebang;
        /* no, we actually found an interpreter name */
        sh = (char *)cp;
        /* look for end of shell/interpreter name */
        while (!ctype(*cp, C_BLANK | C_NUL))
            ++cp;
        /* any arguments? */
        if (*cp) {
            *cp++ = '\0';
            /* skip spaces before arguments */
            while (ctype(*cp, C_BLANK))
                ++cp;
            /* pass it all in ONE argument (historic reasons) */
            if (*cp)
                *tp->args-- = (char *)cp;
        }
        goto nomagic;
    noshebang:
        m = (unsigned)buf[0] << 8 | (unsigned)buf[1];
        if (m == 0x7F45 && buf[2] == 'L' && buf[3] == 'F')
            kerrf0(KWF_ERR(126) | KWF_PREFIX | KWF_FILELINE | KWF_NOERRNO,
                   "%s: not executable: %u-bit ELF file", tp->str, 32U * buf[4]);
    nomagic:;
    }
    args.ro = tp->args;
    *args.ro = sh;
    cap.ro = ap;
    {
        pid_t cpid;
        int   wstatus, rc;

        rc = posix_spawn(&cpid, args.rw[0], NULL, NULL, args.rw, cap.rw);
        if (rc != 0) {
            /* report both the program that was run and the interpreter */
            kerrf(KWF_VERRNO | KWF_ERR(127) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG,
                  rc, tp->str, sh);
        }
        if (waitpid(cpid, &wstatus, 0) < 0)
            exstat = errno;
        else if (WIFEXITED(wstatus))
            exstat = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus))
            exstat = qsh_sigmask(WTERMSIG(wstatus));
        else
            exstat = 1;
        unwind(LLEAVE);
    }
}

/* actual 'builtin' built-in utility call is handled in comexec() */
int
c_builtin(const char **wp)
{
    return (call_builtin(get_builtin(*wp), wp, Tbuiltin, false));
}

struct tbl *
get_builtin(const char *s)
{
    return (s && *s ? ktsearch(&builtins, s, hash(s)) : NULL);
}

/*
 * Search function tables for a function. If create set, a table entry
 * is created if none is found.
 */
struct tbl *
findfunc(const char *name, k32 h, bool create)
{
    struct block *l;
    struct tbl *tp = NULL;

    for (l = e->loc; l; l = l->next) {
        tp = ktsearch(&l->funs, name, h);
        if (tp)
            break;
        if (!l->next && create) {
            tp = ktenter(&l->funs, name, h);
            tp->flag = DEFINED;
            tp->type = CFUNC;
            tp->val.t = NULL;
            break;
        }
    }
    return (tp);
}

/*
 * define function. Returns 1 if function is being undefined (t == 0) and
 * function did not exist, returns 0 otherwise.
 */
int
define(const char *name, struct op *t)
{
    k32 nhash;
    struct tbl *tp;
    bool was_set = false;

    nhash = hash(name);

    while (/* CONSTCOND */ 1) {
        tp = findfunc(name, nhash, true);

        if (tp->flag & ISSET)
            was_set = true;
        /*
         * If this function is currently being executed, we zap
         * this table entry so findfunc() won't see it
         */
        if (tp->flag & FINUSE) {
            tp->name[0] = '\0';
            /* ensure it won't be found */
            tp->flag &= ~DEFINED;
            tp->flag |= FDELETE;
        } else
            break;
    }

    if (tp->flag & ALLOC) {
        tp->flag &= ~(ISSET | ALLOC | FKSH);
        tfree(tp->val.t, tp->areap);
    }

    if (t == NULL) {
        /* undefine */
        ktdelete(tp);
        return (was_set ? 0 : 1);
    }

    tp->val.t = tcopy(t->left, tp->areap);
    tp->flag |= (ISSET | ALLOC);
    if (t->u.qsh_func)
        tp->flag |= FKSH;

    return (0);
}

/*
 * add builtin
 */
const char *
builtin(const char *name, int (*func)(const char **))
{
    struct tbl *tp;
    kui flag = DEFINED;

    /* see if any flags should be set for this builtin */
flags_loop:
    switch (*name) {
    case '=':
        /* command does variable assignment */
        flag |= KEEPASN;
        break;
    case '*':
        /* POSIX special builtin */
        flag |= SPEC_BI;
        break;
    case '~':
        /* external utility overrides built-in utility, always */
        flag |= LOWER_BI;
        /* FALLTHROUGH */
    case '!':
        /* external utility overrides built-in utility, with flags */
        flag |= LOW_BI;
        break;
    case '-':
        /* is declaration utility if argv[1] is one (POSIX: command) */
        flag |= DECL_FWDR;
        break;
    case '^':
        /* is declaration utility (POSIX: export, readonly) */
        flag |= DECL_UTIL;
        break;
    case '#':
        /* is set or shift */
        flag |= NEXTLOC_BI;
        break;
    default:
        goto flags_seen;
    }
    ++name;
    goto flags_loop;
flags_seen:

    /* enter into the builtins hash table */
    tp = ktenter(&builtins, name, hash(name));
    tp->flag = flag;
    tp->type = CSHELL;
    tp->val.f = func;

    /* return name, for direct builtin call check in main.c */
    return (name);
}

/*
 * find command
 * either function, hashed command, or built-in (in that order)
 */
struct tbl *
findcom(const char *name, int flags)
{
    static union tbl_static temp;
    k32 h = hash(name);
    struct tbl *tp = NULL, *tbi;
    /* insert if not found */
    unsigned char insert = Flag(FTRACKALL);
    /* for function autoloading */
    char *fpath;
    union qsh_cchack npath;

    if (qsh_vdirsep(name)) {
        insert = 0;
        /* prevent FPATH search below */
        flags &= ~FC_FUNC;
        goto Search;
    }
    tbi = (flags & FC_BI) ? ktsearch(&builtins, name, h) : NULL;
    /*
     * POSIX says special builtins first, then functions, then
     * regular builtins, then search path...
     */
    if ((flags & FC_SPECBI) && tbi && (tbi->flag & SPEC_BI))
        tp = tbi;
    if (!tp && (flags & FC_FUNC)) {
        tp = findfunc(name, h, false);
        if (tp && !(tp->flag & ISSET)) {
            if ((fpath = str_val(global(TFPATH))) == null) {
                tp->u.fpath = NULL;
                tp->u2.errnov = ENOENT;
            } else
                tp->u.fpath = search_path(name, fpath, R_OK, &tp->u2.errnov);
        }
    }
    if (!tp && (flags & FC_NORMBI) && tbi)
        tp = tbi;
    if (!tp && (flags & FC_PATH) && !(flags & FC_DEFPATH)) {
        tp = ktsearch(&taliases, name, h);
        if (tp && (tp->flag & ISSET) && qsh_access(tp->val.s, X_OK) != 0) {
            if (tp->flag & ALLOC) {
                tp->flag &= ~ALLOC;
                afree(tp->val.s, APERM);
            }
            tp->flag &= ~ISSET;
        }
    }

Search:
    if ((!tp || (tp->type == CTALIAS && !(tp->flag & ISSET))) && (flags & FC_PATH)) {
        if (!tp) {
            if (insert && !(flags & FC_DEFPATH)) {
                tp = ktenter(&taliases, name, h);
                tp->type = CTALIAS;
            } else {
                tp = (struct tbl *)&temp;
                tp->type = CEXEC;
            }
            /* make ~ISSET */
            tp->flag = DEFINED;
        }
        npath.ro = search_path(name, (flags & FC_DEFPATH) ? def_path : path, X_OK, &tp->u2.errnov);
        if (npath.ro) {
            strdupx(tp->val.s, npath.ro, APERM);
            if (npath.ro != name)
                afree(npath.rw, ATEMP);
            tp->flag |= ISSET | ALLOC;
        } else if ((flags & FC_FUNC) && (fpath = str_val(global(TFPATH))) != null &&
                   (npath.ro = search_path(name, fpath, R_OK, &tp->u2.errnov)) != NULL) {
            /*
             * An undocumented feature of AT&T ksh is that
             * it searches FPATH if a command is not found,
             * even if the command hasn't been set up as an
             * autoloaded function (ie, no typeset -uf).
             */
            tp = (struct tbl *)&temp;
            tp->type = CFUNC;
            /* make ~ISSET */
            tp->flag = DEFINED;
            tp->u.fpath = npath.ro;
        }
    }
    return (tp);
}

/*
 * flush executable commands with relative paths
 * (just relative or all?)
 */
void
flushcom(bool all)
{
    struct tbl *tp;
    struct tstate ts;

    for (ktwalk(&ts, &taliases); (tp = ktnext(&ts)) != NULL;)
        if ((tp->flag & ISSET) && (all || !qsh_abspath(tp->val.s))) {
            if (tp->flag & ALLOC) {
                tp->flag &= ~(ALLOC | ISSET);
                afree(tp->val.s, APERM);
            }
            tp->flag &= ~ISSET;
        }
}

/* check if path is something we want to find */
int
search_access(const char *fn, int mode)
{
    struct stat sb;

    if (stat(fn, &sb) < 0)
        /* file does not exist */
        return (ENOENT);
    /* LINTED use of access */
    if (access(fn, mode) < 0) {
        /* file exists, but we can't access it */
        int eno;

        eno = errno;
        return (eno ? eno : EACCES);
    }
    if (mode == X_OK && (!S_ISREG(sb.st_mode) || !(sb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))))
        /* access(2) may say root can execute everything */
        return (S_ISDIR(sb.st_mode) ? EISDIR : EACCES);
    return (0);
}

#define search_access(fn, mode) (search_access)((fn), (mode))

/*
 * search for command with PATH
 */
const char *
search_path(const char *name, const char *lpath,
            /* R_OK or X_OK */
            int mode,
            /* set if candidate found, but not suitable */
            int *errnop)
{
    const char *sp, *p;
    char *xp;
    XString xs;
    size_t namelen;
    int ec = 0, ev;

    if (qsh_vdirsep(name)) {
        if ((ec = search_access(name, mode)) == 0) {
        search_path_ok:
            if (errnop)
                *errnop = 0;
            return (name);
        }
        goto search_path_err;
    }

    namelen = strlen(name) + 1;
    Xinit(xs, xp, 128, ATEMP);

    sp = lpath;
    while (sp != NULL) {
        xp = Xstring(xs, xp);
        if (!(p = cstrchr(sp, QSH_PATHSEPC)))
            p = strnul(sp);
        if (p != sp) {
            XcheckN(xs, xp, p - sp);
            memcpy(xp, sp, p - sp);
            xp += p - sp;
            if (qsh_cdirsep(xp[-1]))
                xp--;
            *xp++ = '/';
        }
        sp = p;
        XcheckN(xs, xp, namelen);
        memcpy(xp, name, namelen);
        if ((ev = search_access(Xstring(xs, xp), mode)) == 0) {
            name = Xclose(xs, xp + namelen);
            goto search_path_ok;
        }
        /* accumulate non-ENOENT errors only */
        if (ev != ENOENT && ec == 0)
            ec = ev;
        if (*sp++ == '\0')
            sp = NULL;
    }
    Xfree(xs, xp);
search_path_err:
    if (errnop)
        *errnop = ec ? ec : ENOENT;
    return (NULL);
}

