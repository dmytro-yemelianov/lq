/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

#define SOFT 0x1
#define HARD 0x2

#undef RLIMIT_CORE /* just in case */

#if defined(UL_GETFSIZE)
#define QSH_UL_GFIL UL_GETFSIZE
#elif defined(UL_GFILLIM)
#define QSH_UL_GFIL UL_GFILLIM
#endif

#if defined(UL_SETFSIZE)
#define QSH_UL_SFIL UL_SETFSIZE
#elif defined(UL_SFILLIM)
#define QSH_UL_SFIL UL_SFILLIM
#endif

#define QSH_UL_WFIL false
#define QSH_UL_SFIL 0

#if defined(UL_GETMAXBRK)
#define QSH_UL_GBRK UL_GETMAXBRK
#elif defined(UL_GMEMLIM)
#define QSH_UL_GBRK UL_GMEMLIM
#endif

#if defined(UL_GDESLIM)
#define QSH_UL_GDES UL_GDESLIM
#endif

extern char etext;
extern long ulimit(int, long);

#define LIMITS_GEN "ulimits.gen"

struct limits {
    /* limit resource / read command */
    int resource;
    /* write command */
    int wesource;
    /* writable? */
    bool writable;
    /* getopts char */
    char optchar;
    /* limit name */
    char name[1];
};

#define RLIMITS_DEFNS
#define FN(lname, lg, ls, lw, lopt)                                                                \
    static const struct {                                                                          \
        int rcmd;                                                                                  \
        int wcmd;                                                                                  \
        bool writable;                                                                             \
        char optchar;                                                                              \
        char name[sizeof(lname)];                                                                  \
    } rlimits_##lg = {lg, ls, lw, lopt, lname};
#include LIMITS_GEN

static void print_ulimit(const struct limits *, int);
static int set_ulimit(const struct limits *, const char *, int);

/*
 * UGH! Strictly speaking this is UB in C. Need to figure out whether
 * it is worth the botherance to fix this (union) or to test for C99+
 * flexible array member using it when present, maybe keeping this if
 * not… :~
 */
static const struct limits *const rlimits[] = {
#define RLIMITS_ITEMS
#include LIMITS_GEN
};

static const char rlimits_opts[] =
#define RLIMITS_OPTCS
#include LIMITS_GEN
#ifndef RLIMIT_CORE
    "c"
#endif
    ;

int
c_ulimit(const char **wp)
{
    size_t i = 0;
    int how = SOFT | HARD, optc;
    char what = 'f';
    bool all = false;

    while ((optc = qsh_getopt(wp, &builtin_opt, rlimits_opts)) != -1)
        switch (optc) {
        case ORD('H'):
            how = HARD;
            break;
        case ORD('S'):
            how = SOFT;
            break;
        case ORD('a'):
            all = true;
            break;
        case ORD('?'):
        unknown_opt:
            kwarnf0(KWF_BIERR | KWF_NOERRNO, "usage: ulimit [-%s] [value]", rlimits_opts);
            return (1);
        default:
            what = optc;
        }

    if (all) {
        if (wp[builtin_opt.optind]) {
        unexpected_args:
            kwarnf(KWF_BIERR | KWF_ONEMSG | KWF_NOERRNO, Ttoo_many_args);
            return (1);
        }
        while (i < NELEM(rlimits)) {
            shprintf("-%c: %-20s  ", rlimits[i]->optchar, rlimits[i]->name);
            print_ulimit(rlimits[i], how);
            ++i;
        }
        return (0);
    }

    while (i < NELEM(rlimits)) {
        if (rlimits[i]->optchar == what)
            goto found;
        ++i;
    }
#ifndef RLIMIT_CORE
    if (what == ORD('c'))
        /* silently accept */
        return (0);
#endif
    qsh_getopt_opterr(what, wp[0], Tunknown_option);
    goto unknown_opt;

found:
    if (wp[builtin_opt.optind]) {
        if (wp[builtin_opt.optind + 1])
            goto unexpected_args;
        return (set_ulimit(rlimits[i], wp[builtin_opt.optind], how));
    }
    print_ulimit(rlimits[i], how);
    return (0);
}

#define RL_T long
#define RL_U LONG_MAX

static int
set_ulimit(const struct limits *l, const char *v, int how QSH_A_UNUSED)
{
    RL_T val = (RL_T)0;
    qiHUGE_U hval;

    if (strcmp(v, "unlimited") == 0) {
        val = RL_U;
        goto got_val;
    }
    if (!getnh(v, &hval)) {
        qsh_uari_t rval;

        if (errno != EINVAL)
            goto inv_val;

        if (!evaluate(v, (qsh_ari_t *)&rval, QSH_RETURN_ERROR, false))
            return (1);
        /*
         * Avoid problems caused by typos that evaluate misses due
         * to evaluating unset parameters to 0...
         * If this causes problems, will have to add parameter to
         * evaluate() to control if unset params are 0 or an error.
         */
        if (!rval && !ctype(v[0], C_DIGIT)) {
            errno = EINVAL;
        inv_val:
            kwarnf0(KWF_BIERR, "invalid %s limit: %s", l->name, v);
            return (1);
        }
        hval = rval;
    }
    errno = EOVERFLOW;
#define qiCfail goto inv_val
    if (qiTYPE_ISU(RL_T))
        qiCASlet(RL_T, val, qiHUGE_U, hval);
    else {
        /* huh, rlim_t is supposed to be unsigned! */
        qiHUGE_S hsval;

        qiCAsafeU2S(qiHUGE_S, qiHUGE_U, hval);
        hsval = qiA_U2S(qiHUGE_U, qiHUGE_S, qiHUGE_S_MAX, hval);
        qiCASlet(RL_T, val, qiHUGE_S, hsval);
    }
#undef qiCfail
    /* do not numerically apprehend magic values */
    if (
        val == RL_U)
        goto inv_val;
got_val:

    if (l->writable == false) {
        /* check.t:ulimit-2 fails if we return 1 and/or do:
        kwarnf(KWF_BIERR | KWF_TWOMSG | KWF_NOERRNO,
            Tread_only, l->name);
        */
        return (0);
    }
    if (ulimit(l->wesource, val) != -1L)
        return (0);
    if (errno == EPERM)
        kwarnf0(KWF_BIERR | KWF_NOERRNO, "%s exceeds allowable %s limit", v, l->name);
    else
        kwarnf0(KWF_BIERR, "%s: bad %s limit", v, l->name);
    return (1);
}

static void
print_ulimit(const struct limits *l, int how QSH_A_UNUSED)
{
    RL_T val = (RL_T)0;
    char numbuf[NUMBUFSZ];
    if ((val = ulimit(l->resource, 0)) < 0)
    {
        shf_puts("unknown", shl_stdout);
        goto out;
    }
    if (val == RL_U)
        shf_puts("unlimited", shl_stdout);
    else {
        shf_puts(qiTYPE_ISU(RL_T) ? kuHfmt((kuH)val, FL_DEC, numbuf)
                                   : ksHfmt((ksH)val, FL_DEC, numbuf),
                 shl_stdout);
    }
out:
    shf_putc('\n', shl_stdout);
}
