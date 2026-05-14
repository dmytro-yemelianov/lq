/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Lookup / alias builtins: whence, command, alias, unalias.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

static int do_whence(const char **, int, bool, bool);

int
c_whence(const char **wp)
{
    int optc;
    bool pflag = false, vflag = false;

    while ((optc = qsh_getopt(wp, &builtin_opt, Tpv)) != -1)
        switch (optc) {
        case 'p':
            pflag = true;
            break;
        case 'v':
            vflag = true;
            break;
        case '?':
            return (1);
        }
    wp += builtin_opt.optind;

    return (do_whence(wp, pflag ? FC_PATH : FC_BI | FC_FUNC | FC_PATH | FC_WHENCE, vflag, false));
}

/* note: command without -vV is dealt with in comexec() */
int
c_command(const char **wp)
{
    int optc, fcflags = FC_BI | FC_FUNC | FC_PATH | FC_WHENCE;
    bool vflag = false;

    while ((optc = qsh_getopt(wp, &builtin_opt, TpVv)) != -1)
        switch (optc) {
        case 'p':
            fcflags |= FC_DEFPATH;
            break;
        case 'V':
            vflag = true;
            break;
        case 'v':
            vflag = false;
            break;
        case '?':
            return (1);
        }
    wp += builtin_opt.optind;

    return (do_whence(wp, fcflags, vflag, true));
}

static int
do_whence(const char **wp, int fcflags, bool vflag, bool iscommand)
{
    k32 h;
    int rv = 0;
    struct tbl *tp;
    const char *id;

    while ((vflag || rv == 0) && (id = *wp++) != NULL) {
        h = hash(id);
        tp = NULL;

        if (fcflags & FC_WHENCE)
            tp = ktsearch(&keywords, id, h);
        if (!tp && (fcflags & FC_WHENCE)) {
            tp = ktsearch(&aliases, id, h);
            if (tp && !(tp->flag & ISSET))
                tp = NULL;
        }
        if (!tp)
            tp = findcom(id, fcflags);

        switch (tp->type) {
        case CSHELL:
        case CFUNC:
        case CKEYWD:
            shf_puts(id, shl_stdout);
            break;
        }

        switch (tp->type) {
        case CSHELL:
            if (vflag)
                shprintf(" is a %sshell %s", (tp->flag & SPEC_BI) ? "special " : "", Tbuiltin);
            break;
        case CFUNC:
            if (vflag) {
                shf_puts(" is a", shl_stdout);
                if (tp->flag & EXPORT)
                    shf_puts("n exported", shl_stdout);
                if (tp->flag & TRACE)
                    shf_puts(" traced", shl_stdout);
                if (!(tp->flag & ISSET)) {
                    shf_puts(" undefined", shl_stdout);
                    if (tp->u.fpath)
                        shprintf(" (autoload from %s)", tp->u.fpath);
                }
                /* shf_puts macro's NULL-check is moot for the address
                 * of an extern array; call shf_write directly to keep
                 * -Waddress quiet. */
                shf_write(T_function, strlen(T_function), shl_stdout);
            }
            break;
        case CEXEC:
        case CTALIAS:
            if (vflag)
                shf_puts(id, shl_stdout);
            if (tp->flag & ISSET) {
                if (vflag) {
                    shf_puts(" is ", shl_stdout);
                    if (tp->type == CTALIAS)
                        shprintf("a tracked %s%s for ", (tp->flag & EXPORT) ? "exported " : "",
                                 Talias);
                }
                if (!qsh_abspath(tp->val.s)) {
                    const char *xcwd = current_wd[0] ? current_wd : Tdot;
                    size_t xlen = strlen(xcwd);
                    size_t clen = strlen(tp->val.s) + 1;
                    char *xp = alloc1(xlen + 1U, clen, ATEMP);

                    memcpy(xp, xcwd, xlen);
                    if (qsh_cdirsep(xp[xlen - 1]))
                        --xlen;
                    xp[xlen++] = '/';
                    memcpy(xp + xlen, tp->val.s, clen);
                    simplify_path(xp);
                    shf_puts(xp, shl_stdout);
                    afree(xp, ATEMP);
                } else
                    shf_puts(tp->val.s, shl_stdout);
            } else {
                if (vflag)
                    shf_puts(Tsp_not_found, shl_stdout);
                rv = 1;
            }
            break;
        case CALIAS:
            if (!vflag && iscommand)
                shprintf(Tf_s_, Talias);
            if (vflag || iscommand)
                print_value_quoted(shl_stdout, id);
            if (vflag)
                shprintf(" is an %s%s for ", (tp->flag & EXPORT) ? "exported " : "", Talias);
            else if (iscommand)
                shf_putc('=', shl_stdout);
            print_value_quoted(shl_stdout, tp->val.s);
            break;
        case CKEYWD:
            if (vflag)
                shf_puts(" is a reserved word", shl_stdout);
            break;
        default:
            bi_errorf(Tunexpected_type, id, Tcommand, tp->type);
            return (1);
        }
        if (vflag || !rv)
            shf_putc('\n', shl_stdout);
    }
    return (rv);
}

bool
valid_alias_name(const char *cp)
{
    switch (ord(*cp)) {
    case ORD('+'):
    case ORD('-'):
        return (false);
    case ORD('['):
        if (ord(cp[1]) == ORD('[') && !cp[2])
            return (false);
        break;
    }
    while (*cp)
        if (ctype(*cp, C_ALIAS))
            ++cp;
        else
            return (false);
    return (true);
}

int
c_alias(const char **wp)
{
    struct table *t = &aliases;
    int rv = 0, prefix = 0;
    bool rflag = false, tflag, Uflag = false, pflag = false, chkalias;
    kui xflag = 0;
    int optc;

    builtin_opt.flags |= GF_PLUSOPT;
    while ((optc = qsh_getopt(wp, &builtin_opt, "dprtUx")) != -1) {
        prefix = builtin_opt.info & GI_PLUS ? '+' : '-';
        switch (optc) {
        case 'd':
            /* QRV: no homedir aliases — `alias -d` is a no-op */
            t = NULL;
            break;
        case 'p':
            pflag = true;
            break;
        case 'r':
            rflag = true;
            break;
        case 't':
            t = &taliases;
            break;
        case 'U':
            /*
             * kludge for tracked alias initialization
             * (don't do a path search, just make an entry)
             */
            Uflag = true;
            break;
        case 'x':
            xflag = EXPORT;
            break;
        case '?':
            return (1);
        }
    }
    if (t == NULL)
        return (0);
    wp += builtin_opt.optind;

    if (!(builtin_opt.info & GI_MINUSMINUS) && *wp && ctype(wp[0][0], C_MINUS | C_PLUS) &&
        wp[0][1] == '\0') {
        prefix = wp[0][0];
        wp++;
    }

    tflag = t == &taliases;
    chkalias = t == &aliases;

    /* "hash -r" means reset all the tracked aliases.. */
    if (rflag) {
        static const char *args[] = {Tunalias, "-ta", NULL};

        if (!tflag || *wp) {
            shprintf("%s: -r flag can only be used with -t"
                     " and without arguments\n",
                     Talias);
            return (1);
        }
        qsh_getopt_reset(&builtin_opt, GF_ERROR);
        return (c_unalias(args));
    }

    if (*wp == NULL) {
        struct tbl *ap, **p;

        for (p = ktsort(t); (ap = *p++) != NULL;)
            if ((ap->flag & (ISSET | xflag)) == (ISSET | xflag)) {
                if (pflag)
                    shprintf(Tf_s_, Talias);
                print_value_quoted(shl_stdout, ap->name);
                if (prefix != '+') {
                    shf_putc('=', shl_stdout);
                    print_value_quoted(shl_stdout, ap->val.s);
                }
                shf_putc('\n', shl_stdout);
            }
    }

    for (; *wp != NULL; wp++) {
        const char *alias = *wp, *val, *newval;
        char *xalias = NULL;
        struct tbl *ap;
        k32 h;

        if ((val = cstrchr(alias, '='))) {
            strndupx(xalias, alias, val++ - alias, ATEMP);
            alias = xalias;
        }
        if (chkalias && !valid_alias_name(alias)) {
            bi_errorf(Tinvname, alias, Talias);
            afree(xalias, ATEMP);
            return (1);
        }
        h = hash(alias);
        if (val == NULL && !tflag && !xflag) {
            ap = ktsearch(t, alias, h);
            if (ap != NULL && (ap->flag & ISSET)) {
                if (pflag)
                    shprintf(Tf_s_, Talias);
                print_value_quoted(shl_stdout, ap->name);
                if (prefix != '+') {
                    shf_putc('=', shl_stdout);
                    print_value_quoted(shl_stdout, ap->val.s);
                }
                shf_putc('\n', shl_stdout);
            } else {
                shprintf(Tf_s_s_sN, alias, Talias, Tnot_found);
                rv = 1;
            }
            continue;
        }
        ap = ktenter(t, alias, h);
        ap->type = tflag ? CTALIAS : CALIAS;
        /* Are we setting the value or just some flags? */
        if ((val && !tflag) || (!val && tflag && !Uflag)) {
            if (ap->flag & ALLOC) {
                ap->flag &= ~(ALLOC | ISSET);
                afree(ap->val.s, APERM);
            }
            /* ignore values for -t (AT&T ksh does this) */
            newval = tflag ? search_path(alias, path, X_OK, NULL) : val;
            if (newval) {
                strdupx(ap->val.s, newval, APERM);
                ap->flag |= ALLOC | ISSET;
            } else
                ap->flag &= ~ISSET;
        }
        ap->flag |= DEFINED;
        if (prefix == '+')
            ap->flag &= ~xflag;
        else
            ap->flag |= xflag;
        afree(xalias, ATEMP);
    }

    return (rv);
}

int
c_unalias(const char **wp)
{
    struct table *t = &aliases;
    struct tbl *ap;
    int optc, rv = 0;
    bool all = false;

    while ((optc = qsh_getopt(wp, &builtin_opt, "adt")) != -1)
        switch (optc) {
        case 'a':
            all = true;
            break;
        case 'd':
            /* QRV: no homedir aliases — `unalias -d` is a no-op */
            t = NULL;
            break;
        case 't':
            t = &taliases;
            break;
        case '?':
            return (1);
        }
    if (t == NULL)
        return (0);
    wp += builtin_opt.optind;

    for (; *wp != NULL; wp++) {
        ap = ktsearch(t, *wp, hash(*wp));
        if (ap == NULL) {
            /* POSIX */
            rv = 1;
            continue;
        }
        if (ap->flag & ALLOC) {
            ap->flag &= ~(ALLOC | ISSET);
            afree(ap->val.s, APERM);
        }
        ap->flag &= ~(DEFINED | ISSET | EXPORT);
    }

    if (all) {
        struct tstate ts;

        for (ktwalk(&ts, t); (ap = ktnext(&ts));) {
            if (ap->flag & ALLOC) {
                ap->flag &= ~(ALLOC | ISSET);
                afree(ap->val.s, APERM);
            }
            ap->flag &= ~(DEFINED | ISSET | EXPORT);
        }
    }

    return (rv);
}
