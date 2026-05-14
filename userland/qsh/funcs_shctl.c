/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Shell-control builtins: let, getopts, shift, umask, dot/source, wait,
 * eval, trap, exit/return, break/continue, set, unset.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

int
c_let(const char **wp)
{
    int rv = 1;
    qsh_ari_t val;

    if (wp[1] == NULL)
        /* AT&T ksh does this */
        bi_errorf(Tno_args);
    else
        for (wp++; *wp; wp++)
            if (!evaluate(*wp, &val, QSH_RETURN_ERROR, true)) {
                /* distinguish error from zero result */
                rv = 2;
                break;
            } else
                rv = val == 0;
    return (rv);
}

void
getopts_reset(int val)
{
    if (val >= 1) {
        qsh_getopt_reset(&user_opt, GF_NONAME | (Flag(FPOSIX) ? 0 : GF_PLUSOPT));
        user_opt.optind = user_opt.uoptind = val;
    }
}

int
c_getopts(const char **wp)
{
    int argc, optc, rv;
    const char *opts, *var;
    char buf[3];
    struct tbl *vq, *voptarg;

    if (qsh_getopt(wp, &builtin_opt, null) == '?')
        return (1);
    wp += builtin_opt.optind;

    opts = *wp++;
    if (!opts) {
        bi_errorf(Tf_sD_s, "options", Tno_args);
        return (1);
    }

    var = *wp++;
    if (!var) {
        bi_errorf(Tf_sD_s, Tname, Tno_args);
        return (1);
    }
    if (!*var || *skip_varname(var, true)) {
        bi_errorf(Tf_sD_s, var, Tnot_ident);
        return (1);
    }

    if (e->loc->next == NULL) {
        kwarnf(KWF_INTERNAL | KWF_WARNING | KWF_TWOMSG | KWF_NOERRNO, Tgetopts, Tno_args);
        return (1);
    }
    /* Which arguments are we parsing... */
    if (*wp == NULL)
        wp = e->loc->next->argv;
    else
        *--wp = e->loc->next->argv[0];

    /* Check that our saved state won't cause a core dump... */
    for (argc = 0; wp[argc]; argc++)
        ;
    if (user_opt.optind > argc ||
        (user_opt.p != 0 && user_opt.p > strlen(wp[user_opt.optind - 1]))) {
        bi_errorf("arguments changed since last call");
        return (1);
    }

    user_opt.optarg = NULL;
    optc = qsh_getopt(wp, &user_opt, opts);

    if (optc >= 0 && optc != '?' && (user_opt.info & GI_PLUS)) {
        buf[0] = '+';
        buf[1] = optc;
        buf[2] = '\0';
    } else {
        /*
         * POSIX says var is set to ? at end-of-options, AT&T ksh
         * sets it to null - we go with POSIX...
         */
        buf[0] = optc < 0 ? '?' : optc;
        buf[1] = '\0';
    }

    /* AT&T ksh93 in fact does change OPTIND for unknown options too */
    user_opt.uoptind = user_opt.optind;

    voptarg = global("OPTARG");
    /* AT&T ksh clears ro and int */
    voptarg->flag &= ~RDONLY;
    /* Paranoia: ensure no bizarre results. */
    if (voptarg->flag & INTEGER)
        typeset("OPTARG", 0, INTEGER, 0, 0);
    if (user_opt.optarg == NULL)
        unset(voptarg, 1);
    else
        /* this can't fail (haing cleared readonly/integer) */
        setstr(voptarg, user_opt.optarg, QSH_RETURN_ERROR);

    rv = 0;

    vq = global(var);
    /* Error message already printed (integer, readonly) */
    if (!setstr(vq, buf, QSH_RETURN_ERROR))
        rv = 2;
    if (Flag(FEXPORT))
        typeset(var, EXPORT, 0, 0, 0);

    return (optc < 0 ? 1 : rv);
}

int
c_shift(const char **wp)
{
    int n;
    qsh_ari_t val;
    const char *arg;
    struct block *l = e->loc;

    if ((l->flags & BF_RESETSPEC)) {
        /* prevent pollution */
        l->flags &= ~BF_RESETSPEC;
        /* operate on parent environment */
        l = l->next;
    }

    if (qsh_getopt(wp, &builtin_opt, null) == '?')
        return (1);
    arg = wp[builtin_opt.optind];

    if (!arg)
        n = 1;
    else if (!evaluate(arg, &val, QSH_RETURN_ERROR, false)) {
        /* error already printed */
        bi_unwind(1);
        return (1);
    } else if (!(n = val)) {
        /* nothing to do */
        return (0);
    } else if (n < 0) {
        bi_errorf(Tf_sD_s, Tbadnum, arg);
        return (1);
    }

    if (l->argc < n) {
        bi_errorf("nothing to shift");
        return (1);
    }
    l->argv[n] = l->argv[0];
    l->argv += n;
    l->argc -= n;
    return (0);
}

int
c_umask(const char **wp)
{
    int i, optc;
    const char *cp;
    bool symbolic = false;
    mode_t old_umask;

    while ((optc = qsh_getopt(wp, &builtin_opt, "S")) != -1)
        switch (optc) {
        case 'S':
            symbolic = true;
            break;
        case '?':
            return (1);
        }
    cp = wp[builtin_opt.optind];
    if (cp == NULL) {
        old_umask = umask((mode_t)0);
        umask(old_umask);
        if (symbolic) {
            char buf[18], *p;
            int j;

            old_umask = ~old_umask;
            p = buf;
            for (i = 0; i < 3; i++) {
                *p++ = Tugo[i];
                *p++ = '=';
                for (j = 0; j < 3; j++)
                    if (old_umask & (1 << (8 - (3 * i + j))))
                        *p++ = "rwx"[j];
                *p++ = ',';
            }
            p[-1] = '\0';
            shprintf(Tf_sN, buf);
        } else
            shprintf("%#3.3o\n", (unsigned int)old_umask);
    } else {
        mode_t new_umask;

        if (ctype(*cp, C_DIGIT)) {
            new_umask = 0;
            while (ctype(*cp, C_OCTAL)) {
                new_umask = new_umask * 8 + qsh_numdig(*cp);
                ++cp;
            }
            if (*cp) {
                bi_errorf(Tbadnum);
                return (1);
            }
        } else {
            /* symbolic format */
            int positions, new_val;
            char op;

            old_umask = umask((mode_t)0);
            /* in case of error */
            umask(old_umask);
            old_umask = ~old_umask;
            new_umask = old_umask;
            positions = 0;
            while (*cp) {
                while (*cp && vstrchr(Taugo, *cp))
                    switch (*cp++) {
                    case 'a':
                        positions |= 0111;
                        break;
                    case 'u':
                        positions |= 0100;
                        break;
                    case 'g':
                        positions |= 0010;
                        break;
                    case 'o':
                        positions |= 0001;
                        break;
                    }
                if (!positions)
                    /* default is a */
                    positions = 0111;
                if (!ctype((op = *cp), C_EQUAL | C_MINUS | C_PLUS))
                    break;
                cp++;
                new_val = 0;
                while (*cp && vstrchr("rwxugoXs", *cp))
                    switch (*cp++) {
                    case 'r':
                        new_val |= 04;
                        break;
                    case 'w':
                        new_val |= 02;
                        break;
                    case 'x':
                        new_val |= 01;
                        break;
                    case 'u':
                        new_val |= old_umask >> 6;
                        break;
                    case 'g':
                        new_val |= old_umask >> 3;
                        break;
                    case 'o':
                        new_val |= old_umask >> 0;
                        break;
                    case 'X':
                        if (old_umask & 0111)
                            new_val |= 01;
                        break;
                    case 's':
                        /* ignored */
                        break;
                    }
                new_val = (new_val & 07) * positions;
                switch (op) {
                case '-':
                    new_umask &= ~new_val;
                    break;
                case '=':
                    new_umask = new_val | (new_umask & ~(positions * 07));
                    break;
                case '+':
                    new_umask |= new_val;
                }
                if (*cp == ',') {
                    positions = 0;
                    cp++;
                } else if (!ctype(*cp, C_EQUAL | C_MINUS | C_PLUS))
                    break;
            }
            if (*cp) {
                bi_errorf("bad mask");
                return (1);
            }
            new_umask = ~new_umask;
        }
        umask(new_umask);
    }
    return (0);
}

int
c_dot(const char **wp)
{
    const char *file, *cp, **argv;
    int rv, errcode;

    if (qsh_getopt(wp, &builtin_opt, null) == '?')
        return (1);

    if ((cp = wp[builtin_opt.optind]) == NULL) {
        bi_errorf(Tno_args);
        return (1);
    }
    file = search_path(cp, path, R_OK, &errcode);
    if (!file && errcode == ENOENT && wp[0][0] == 's' && search_access(cp, R_OK) == 0)
        file = cp;
    if (!file) {
        bi_errorf(Tf_sD_s, cp, cstrerror(errcode));
        return (1);
    }

    /* Set positional parameters? */
    if (wp[builtin_opt.optind + 1]) {
        argv = wp + builtin_opt.optind;
        /* preserve $0 */
        argv[0] = e->loc->argv[0];
    } else
        argv = NULL;
    /* SUSv4: OR with a high value never written otherwise */
    exstat |= 0x4000;
    if ((rv = include(file, argv, false)) < 0) {
        if (__predict_true(rv == -2)) {
            /* error already printed */
            bi_unwind(1);
        } else {
            /* should not happen */
            kwarnf(KWF_BIERR | KWF_ONEMSG, cp);
        }
        return (1);
    }
    if (exstat & 0x4000)
        /* detect old exstat, use 0 in that case */
        rv = 0;
    return (rv);
}

int
c_wait(const char **wp)
{
    int rv = 0, sig;

    if (qsh_getopt(wp, &builtin_opt, null) == '?')
        return (1);
    wp += builtin_opt.optind;
    if (*wp == NULL) {
        while (waitfor(NULL, &sig) >= 0)
            ;
        rv = sig;
    } else {
        for (; *wp; wp++)
            rv = waitfor(*wp, &sig);
        if (rv < 0)
            /* magic exit code: bad job-id */
            rv = sig ? sig : 127;
    }
    return (rv);
}

int
c_eval(const char **wp)
{
    if (qsh_getopt(wp, &builtin_opt, null) == '?')
        return (1);
    return (do_evalcmd(wp + builtin_opt.optind));
}

int
do_evalcmd(const char **wp)
{
    struct source *s, *saves = source;
    int rv;

    s = pushs(SWORDS, ATEMP);
    s->u.strv = wp;
    s->line = current_lineno;

    /*-
     * The following code handles the case where the command is
     * empty due to failed command substitution, for example by
     *  eval "$(false)"
     * This has historically returned 1 by AT&T ksh88. In this
     * case, shell() will not set or change exstat because the
     * compiled tree is empty, so it will use the value we pass
     * from subst_exstat, which is cleared in execute(), so it
     * should have been 0 if there were no substitutions.
     *
     * POSIX however says we don't do this, even though it is
     * traditionally done. AT&T ksh93 agrees with POSIX, so we
     * do. The following is an excerpt from SUSv4 [1003.2-2008]:
     *
     * 2.9.1: Simple Commands
     *  ... If there is a command name, execution shall
     *  continue as described in 2.9.1.1 [Command Search
     *  and Execution]. If there is no command name, but
     *  the command contained a command substitution, the
     *  command shall complete with the exit status of the
     *  last command substitution performed.
     * 2.9.1.1: Command Search and Execution
     *  (1) a. If the command name matches the name of a
     *  special built-in utility, that special built-in
     *  utility shall be invoked.
     * 2.14.5: eval
     *  If there are no arguments, or only null arguments,
     *  eval shall return a zero exit status; ...
     */
    /* AT&T ksh88: use subst_exstat */
    /* exstat = subst_exstat; */
    /* SUSv4: OR with a high value never written otherwise */
    exstat |= 0x4000;

    rv = shell(s, 2);
    source = saves;
    afree(s, ATEMP);
    if (exstat & 0x4000)
        /* detect old exstat, use 0 in that case */
        rv = 0;
    return (rv);
}

int
c_trap(const char **wp)
{
    Trap *p = sigtraps;
    int i = qsh_NSIG;
    const char *s;

    if (qsh_getopt(wp, &builtin_opt, null) == '?')
        return (1);
    wp += builtin_opt.optind;

    if (*wp == NULL) {
        do {
            if (p->trap) {
                shf_puts("trap -- ", shl_stdout);
                print_value_quoted(shl_stdout, p->trap);
                shprintf(Tf__sN, p->name);
            }
            ++p;
        } while (i--);
        return (0);
    }

    if (getn(*wp, &i)) {
        /* first argument is a signal number, reset them all */
        s = NULL;
    } else {
        /* first argument must be a command, then */
        s = *wp++;
        /* reset traps? */
        if (qsh_isdash(s))
            s = NULL;
    }

    /* set/clear the traps */
    i = 0;
    while (*wp)
        if (!(p = gettrap(*wp++, true, true))) {
            kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_BUILTIN | KWF_TWOMSG | KWF_NOERRNO, Tbad_sig,
                   wp[-1]);
            i = 1;
        } else
            settrap(p, s);
    return (i);
}

int
c_exitreturn(const char **wp)
{
    int n, how = LEXIT;

    if (wp[1]) {
        if (wp[2])
            goto c_exitreturn_err;
        exstat = bi_getn(wp[1], &n) ? (n & 0xFF) : 1;
    } else if (trap_exstat != -1)
        exstat = trap_exstat;

    if (wp[0][0] == 'r') {
        /* return */
        struct env *ep;

        /*
         * need to tell if this is exit or return so trap exit will
         * work right (POSIX)
         */
        for (ep = e; ep; ep = ep->oenv)
            if (STOP_RETURN(ep->type)) {
                how = LRETURN;
                break;
            }
    }

    if (how == LEXIT && !really_exit && j_stopped_running()) {
        really_exit = true;
        how = LSHELL;
    }

    /* get rid of any I/O redirections */
    quitenv(NULL);
    unwind(how);
    /* NOTREACHED */

c_exitreturn_err:
    bi_errorf(Ttoo_many_args);
    return (1);
}

int
c_brkcont(const char **wp)
{
    unsigned int quit;
    int n;
    struct env *ep, *last_ep = NULL;
    const char *arg;

    if (qsh_getopt(wp, &builtin_opt, null) == '?')
        goto c_brkcont_err;
    arg = wp[builtin_opt.optind];

    if (!arg)
        n = 1;
    else if (!bi_getn(arg, &n))
        goto c_brkcont_err;
    if (n <= 0) {
        /* AT&T ksh does this for non-interactive shells only - weird */
        bi_errorf("%s: bad value", arg);
        goto c_brkcont_err;
    }
    quit = (unsigned int)n;

    /* Stop at E_NONE, E_PARSE, E_FUNC, or E_INCL */
    for (ep = e; ep && !STOP_BRKCONT(ep->type); ep = ep->oenv)
        if (ep->type == E_LOOP) {
            if (--quit == 0)
                break;
            ep->flags |= EF_BRKCONT_PASS;
            last_ep = ep;
        }

    if (quit) {
        /*
         * AT&T ksh doesn't print a message - just does what it
         * can. We print a message 'cause it helps in debugging
         * scripts, but don't generate an error (ie, keep going).
         */
        if ((unsigned int)n == quit) {
            kwarnf0(KWF_PREFIX | KWF_FILELINE | KWF_NOERRNO, Tf_cant_s, wp[0], wp[0]);
            return (0);
        }
        /*
         * POSIX says if n is too big, the last enclosing loop
         * shall be used. Doesn't say to print an error but we
         * do anyway 'cause the user messed up.
         */
        if (last_ep)
            last_ep->flags &= ~EF_BRKCONT_PASS;
        kwarnf0(KWF_PREFIX | KWF_FILELINE | KWF_NOERRNO, "%s: can only %s %u level(s)", wp[0],
                wp[0], (unsigned int)n - quit);
    }

    unwind(*wp[0] == 'b' ? LBREAK : LCONTIN);
    /* NOTREACHED */

c_brkcont_err:
    return (1);
}

int
c_set(const char **wp)
{
    int argi;
    bool setargs;
    struct block *l = e->loc;

    if ((l->flags & BF_RESETSPEC)) {
        /* prevent pollution */
        l->flags &= ~BF_RESETSPEC;
        /* operate on parent environment */
        l = l->next;
    }

    if (wp[1] == NULL) {
        static const char *args[] = {Tset, "-", NULL};
        return (c_typeset(args));
    }

    if ((argi = parse_args(wp, OF_SET, &setargs)) < 0)
        return (2);
    /* set $# and $* */
    if (setargs) {
        wp += argi - 1;
        /* save $0 */
        wp[0] = l->argv[0];
        l->argv = cpyargv(&l->argc, wp, &l->area);
    }
    /*-
     * POSIX says set exit status is 0, but old scripts that use
     * getopt(1) use the construct
     *  set -- $(getopt ab:c "$@")
     * which assumes the exit value set will be that of the $()
     * (subst_exstat is cleared in execute() so that it will be 0
     * if there are no command substitutions).
     */
    /* conformant behaviour, unless set -o sh +o posix */
    return (Flag(FSH) && !Flag(FPOSIX) ? subst_exstat : 0);
}

int
c_unset(const char **wp)
{
    const char *id;
    int optc, rv = 0;
    bool unset_var = true;

    while ((optc = qsh_getopt(wp, &builtin_opt, "fv")) != -1)
        switch (optc) {
        case 'f':
            unset_var = false;
            break;
        case 'v':
            unset_var = true;
            break;
        case '?':
            /*XXX not reached due to GF_ERROR in spec_bi */
            return (2);
        }
    wp += builtin_opt.optind;
    for (; (id = *wp) != NULL; wp++)
        if (unset_var) {
            /* unset variable */
            struct tbl *vp;
            char *cp = NULL;
            size_t n;

            n = strlen(id);
            if (n > 3 && ord(id[n - 3]) == ORD('[') && ord(id[n - 2]) == ORD('*') &&
                ord(id[n - 1]) == ORD(']')) {
                strndupx(cp, id, n - 3, ATEMP);
                id = cp;
                optc = 3;
            } else
                optc = vstrchr(id, '[') ? 0 : 1;

            vp = global(id);
            afree(cp, ATEMP);

            if ((vp->flag & RDONLY)) {
                kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, Tread_only, vp->name);
                rv = 1;
            } else
                unset(vp, optc);
        } else
            /* unset function */
            define(id, NULL);
    return (rv);
}
