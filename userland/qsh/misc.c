/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

static const unsigned char *pat_scan(const unsigned char *, const unsigned char *, bool);
static int do_gmatch(const unsigned char *, const unsigned char *, const unsigned char *,
                     const unsigned char *, const unsigned char *);
static const unsigned char *gmatch_cclass(const unsigned char *, unsigned char);
static unsigned int dollarqU(struct shf *, const unsigned char *);
static void dollarq8(struct shf *, const unsigned char *);

/*XXX this should go away */
static int make_path(const char *, const char *, char **, XString *, int *);

#define DO_SETUID(func, argvec) func argvec

/* called from XcheckN() to grow buffer */
char *
Xcheck_grow(XString *xsp, const char *xp, size_t more)
{
    size_t old_ofs = xp - xsp->beg;

    if (more < xsp->len)
        more = xsp->len;
    /* (xsp->len + X_EXTRA) never overflows */
    checkoktoadd(more, xsp->len + X_EXTRA);
    xsp->beg = aresize(xsp->beg, (xsp->len += more) + X_EXTRA, xsp->areap);
    xsp->end = xsp->beg + xsp->len;
    return (xsp->beg + old_ofs);
}

#define SHFLAGS_DEFNS
#define FN(sname, cname, flags, ochar)                                                             \
    static const struct shoptionS_##cname {                                                        \
        /* character flag (if any) */                                                              \
        char c;                                                                                    \
        /* OF_* */                                                                                 \
        unsigned char optflags;                                                                    \
        /* long name of option */                                                                  \
        char name[sizeof(sname)];                                                                  \
    } shoptione_##cname = {ochar, flags, sname};
#include "sh_flags.gen"

qCTA_BEG(sh_flags_gen);
#define FN(sname, cname, flags, ochar)                                                             \
    qccCTA(o_##cname, (offsetof(struct shoptionS_##cname, optflags) == 1 &&                       \
                        offsetof(struct shoptionS_##cname, name) == 2));
#include "sh_flags.gen"
qCTA_END(sh_flags_gen);

#define OFC(i) (options[i][-2])
#define OFF(i) (((const unsigned char *)options[i])[-1])
#define OFN(i) (options[i])

const char *const options[] = {
#define SHFLAGS_ITEMS
#include "sh_flags.gen"
};

/*
 * translate -o option into F* constant (also used for test -o option)
 */
size_t
option(const char *n)
{
    size_t i = 0;

    if (ctype(n[0], C_MINUS | C_PLUS) && n[1] && !n[2])
        while (i < NELEM(options)) {
            if (OFC(i) == n[1])
                return (i);
            ++i;
        }
    else
        while (i < NELEM(options)) {
            if (!strcmp(OFN(i), n))
                return (i);
            ++i;
        }

    return ((size_t)-1);
}

struct options_info {
    int opt_width;
    int opts[NELEM(options)];
};

static void options_fmt_entry(char *, size_t, unsigned int, const void *);
static int printoptions(bool);
static int printoption(size_t);

/* format a single select menu item */
static void
options_fmt_entry(char *buf, size_t buflen, unsigned int i, const void *arg)
{
    const struct options_info *oi = (const struct options_info *)arg;

    shf_snprintf(buf, buflen, "%-*s %s", oi->opt_width, OFN(oi->opts[i]),
                 Flag(oi->opts[i]) ? "on" : "off");
}

static int
printoption(size_t i)
{
    if (Flag(i) == baseline_flags[i])
        return (0);
    if (!OFN(i)[0]) {
        bi_errorf(Tf_sd, "change in unnamed option", (int)i);
        return (1);
    }
    if (Flag(i) != 0 && Flag(i) != 1) {
        bi_errorf(Tf_s_sD_s, Tdo, OFN(i), "not 0 or 1");
        return (1);
    }
    shprintf(Tf__s_s, Flag(i) ? Tdo : Tpo, OFN(i));
    return (0);
}

static int
printoptions(bool verbose)
{
    size_t i = 0;
    int rv = 0;

    if (verbose) {
        size_t n = 0, len, octs = 0;
        struct options_info oi;
        struct columnise_opts co;

        /* verbose version */
        shf_puts("Current option settings\n", shl_stdout);

        oi.opt_width = 0;
        while (i < NELEM(options)) {
            if ((len = strlen(OFN(i)))) {
                oi.opts[n++] = i;
                if (len > octs)
                    octs = len;
                len = utf_mbswidth(OFN(i));
                if ((int)len > oi.opt_width)
                    oi.opt_width = (int)len;
            }
            ++i;
        }
        co.shf = shl_stdout;
        co.linesep = '\n';
        co.prefcol = co.do_last = true;
        print_columns(&co, n, options_fmt_entry, &oi, octs + 4, oi.opt_width + 4);
    } else {
        /* short version like AT&T ksh93 */
        shf_puts(Tset, shl_stdout);
        /* shf_puts macro's NULL-check is moot for the address of an
         * extern array; call shf_write directly to keep -Waddress quiet. */
        shf_write(To_o_reset, strlen(To_o_reset), shl_stdout);
        printoption(FSH);
        printoption(FPOSIX);
        while (i < FNFLAGS) {
            if (i != FSH && i != FPOSIX)
                rv |= printoption(i);
            ++i;
        }
        shf_putc('\n', shl_stdout);
    }
    return (rv);
}

char *
getoptions(void)
{
    size_t i = 0;
    char c, m[(int)FNFLAGS + 1];
    char *cp = m;

    while (i < NELEM(options)) {
        if ((c = OFC(i)) && Flag(i))
            *cp++ = c;
        ++i;
    }
    strndupx(cp, m, cp - m, ATEMP);
    return (cp);
}

/* change a Flag(*) value; takes care of special actions */
void
change_flag(enum sh_flag f,
            /* OF_INTERNAL, OF_FIRSTTIME, OF_CMDLINE, or OF_SET */
            unsigned int what, bool newset)
{
    unsigned char oldval = Flag(f);
    unsigned char newval = (newset ? 1 : 0);

    if (f == FXTRACE) {
        change_xtrace(newval, true);
        return;
    } else if (f == FPRIVILEGED) {
        if (!oldval)
            /* no getting back dropped privs */
            return;
        else if (!newval) {
            /* turning off -p */
            qshegid = qshgid;
            qsheuid = qshuid;
        } else if (oldval != 3)
            /* nor going full sugid */
            goto change_flag;

        /* +++ set group IDs +++ */
        /* setgid, setegid don't EAGAIN on Linux */
        setgid(qshegid);

        /* +++ wipe groups vector +++ */

        /* +++ set user IDs +++ */
        /* seteuid doesn't EAGAIN on Linux */
        DO_SETUID(setuid, (qsheuid));

        /* +++ privs changed +++ */
    } else if ((f == FPOSIX || f == FSH) && newval) {
        /* Turning on -o posix or -o sh? */
        Flag(FBRACEEXPAND) = 0;
    }
    /* QRV: no FEMACS/FVI/FGMACS mutual exclusion — editor is hardcoded. */

change_flag:
    Flag(f) = newval;

    if (f == FTALKING) {
        /* Changing interactive flag? */
        if (what == OF_CMDLINE && procpid == qshpid)
            Flag(FTALKING_I) = newval;
    }
}

void
change_xtrace(unsigned char newval, bool dosnapshot)
{
    static bool in_xtrace;

    if (in_xtrace)
        return;

    if (!dosnapshot && newval == Flag(FXTRACE))
        return;

    if (Flag(FXTRACE) == 2) {
        shf_putc('\n', shl_xtrace);
        Flag(FXTRACE) = 1;
        shf_flush(shl_xtrace);
    }

    if (!dosnapshot && Flag(FXTRACE) == 1)
        switch (newval) {
        case 1:
            return;
        case 2:
            goto changed_xtrace;
        }

    shf_flush(shl_xtrace);
    if (shl_xtrace->fd != 2)
        close(shl_xtrace->fd);
    if (!newval || (shl_xtrace->fd = savefd(2)) == -1)
        shl_xtrace->fd = 2;

changed_xtrace:
    if ((Flag(FXTRACE) = newval) == 2) {
        in_xtrace = true;
        Flag(FXTRACE) = 0;
        shf_putsv(substitute(str_val(global("PS4")), 0), shl_xtrace);
        Flag(FXTRACE) = 2;
        in_xtrace = false;
    }
}

/*
 * Parse command line and set command arguments. Returns the index of
 * non-option arguments, -1 if there is an error.
 */
int
parse_args(const char **argv,
           /* OF_FIRSTTIME, OF_CMDLINE, or OF_SET */
           unsigned int what, bool *setargsp)
{
    static const char cmd_opts[] =
#define SHFLAGS_NOT_SET
#define SHFLAGS_OPTCS
#include "sh_flags.gen"
#undef SHFLAGS_NOT_SET
        ;
    static const char set_opts[] =
#define SHFLAGS_NOT_CMD
#define SHFLAGS_OPTCS
#include "sh_flags.gen"
#undef SHFLAGS_NOT_CMD
        ;
    bool set;
    const char *const opts = what == OF_SET ? set_opts : cmd_opts;
    const char *array = NULL;
    Getopt go;
    size_t i;
    int optc, arrayset = 0;
    bool sortargs = false;
    bool fcompatseen = false;

    qsh_getopt_reset(&go, GF_ERROR | GF_PLUSOPT);
    while ((optc = qsh_getopt(argv, &go, opts)) != -1) {
        set = ((bool)(!(go.info & GI_PLUS)));
        switch (optc) {
        case 'A':
            if (what == OF_FIRSTTIME)
                break;
            arrayset = set ? 1 : -1;
            array = go.optarg;
            break;

        case 'o':
            if (what == OF_FIRSTTIME)
                break;
            if (go.optarg == NULL) {
                /*
                 * lone -o: print options
                 *
                 * Note that on the command line, -o requires
                 * an option (i.e. can't get here if what is
                 * not OF_SET).
                 */
                if (!set && !baseline_flags[(int)FNFLAGS]) {
                    bi_errorf(Ttooearly, T_set_po);
                    return (-1);
                }
                if (printoptions(set))
                    return (-1);
                break;
            }
            i = option(go.optarg);
            if ((i == FPOSIX || i == FSH) && set && !fcompatseen) {
                /*
                 * If running 'set -o posix' or
                 * 'set -o sh', turn off the other;
                 * if running 'set -o posix -o sh'
                 * allow both to be set though.
                 */
                Flag(FPOSIX) = 0;
                Flag(FSH) = 0;
                fcompatseen = true;
            }
            if ((i != (size_t)-1) && (set ? 1U : 0U) == Flag(i))
                /*
                 * Don't check the context if the flag
                 * isn't changing - makes "set -o interactive"
                 * work if you're already interactive. Needed
                 * if the output of "set +o" is to be used.
                 */
                ;
            else if ((i != (size_t)-1) && (OFF(i) & what))
                change_flag((enum sh_flag)i, what, set);
            else if (!strcmp(go.optarg, To_reset)) {
                if (!baseline_flags[(int)FNFLAGS]) {
                    bi_errorf(Ttooearly, To_o_reset);
                    return (-1);
                }
                /*
                 * ordering, with respect to side effects,
                 * was ensured above by printoptions
                 */
                for (i = 0; i < FNFLAGS; ++i)
                    if (Flag(i) != baseline_flags[i])
                        change_flag((enum sh_flag)i, what, baseline_flags[i]);
            } else {
                bi_errorf(Tf_sD_s, go.optarg, Tunknown_option);
                return (-1);
            }
            break;

        /* QRV: -T (chvt — change vt + reattach) removed. */

        case '?':
            return (-1);

        default:
            if (what == OF_FIRSTTIME)
                break;
            /* -s: sort positional params (via AT&T ksh) */
            if (what == OF_SET && isch(optc, 's')) {
                sortargs = true;
                break;
            }
            for (i = 0; i < NELEM(options); i++)
                if (optc == OFC(i) && (what & OFF(i))) {
                    change_flag((enum sh_flag)i, what, set);
                    break;
                }
            if (i == NELEM(options))
                kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, "parse_args: '%c'", optc);
        }
    }
    /* lone ‘-’ (or ‘+’)? */
    if (argv[go.optind] && ctype(argv[go.optind][0], C_MINUS | C_PLUS) &&
        argv[go.optind][1] == '\0' && !(go.info & GI_MINUSMINUS)) {
        /* POSIX: lone hyphen-minus sh first arg ignored */
        if (what == OF_SET && isch(argv[go.optind][0], '-')) {
            /* set; lone dash clears -v and -x flags (obsolete) */
            Flag(FVERBOSE) = 0;
            change_xtrace(0, false);
        }
        /* either way, skip it (POSIX only dash but… meh) */
        go.optind++;
    }
    if (setargsp)
        /* -- means set $#/$* even if there are no arguments */
        *setargsp = !arrayset && ((go.info & GI_MINUSMINUS) || argv[go.optind]);

    if (arrayset) {
        const char *ccp = NULL;

        if (array && *array)
            ccp = skip_varname(array, false);
        if (!ccp || !(!ccp[0] || (ccp[0] == '+' && !ccp[1]))) {
            bi_errorf(Tf_sD_s, array, Tnot_ident);
            return (-1);
        }
    }
    if (sortargs) {
        for (i = go.optind; argv[i]; i++)
            ;
        qsort(&argv[go.optind], i - go.optind, sizeof(void *), ascpstrcmp);
    }
    if (arrayset)
        go.optind += set_array(array, ((bool)(arrayset > 0)), argv + go.optind);

    return (go.optind);
}

/* parse a decimal number: returns 0 if string isn't a number, 1 otherwise */
int
getn(const char *s, int *ai)
{
    if (!getpn(&s, ai))
        return (0);
    if (!*s)
        return (1);
    errno = EINVAL;
    return (0);
}

int
getnh(const char *s, qiHUGE_U *ai)
{
    if (!getpnh(&s, ai))
        return (0);
    if (!*s)
        return (1);
    errno = EINVAL;
    return (0);
}

/*
 * parse a decimal number
 * on success, returns 1 and *ai contains it and *sp points past it
 * on overflow, returns 0, *ai is 0, *sp points behind the parsed number
 * on parse error (not numeric) returns 0, *ai is 0, *sp is not changed
 */
int
getpn(const char **sp, int *ai)
{
    char c;
    const char *s;
    qsh_uari_t num = 0;
    kby state = 0;
    bool neg = false;

    s = *sp;

    do {
        c = *s++;
    } while (ctype(c, C_SPACE));

    switch (c) {
    case '-':
        neg = true;
        /* FALLTHROUGH */
    case '+':
        c = *s++;
        break;
    }

    while (ctype(c, C_DIGIT)) {
        /*XXX this supposed to return int? */
        if (num > 214748364U) {
            /* overflow on multiplication */
            state = 2;
            errno = EOVERFLOW;
        }
        if (state < 2) {
            state = 1;
            num = num * 10U + (unsigned int)qsh_numdig(c);
        }
        /* now: num <= 2147483649U */
        c = *s++;
    }
    --s;

    if (num > (neg ? 2147483648U : 2147483647U)) {
        /* overflow for signed 32-bit int */
        state = 2;
        errno = EOVERFLOW;
    }

    if (state)
        *sp = s;
    else
        errno = EINVAL;
    if (state != 1) {
        *ai = 0;
        return (0);
    }
    /*XXX this supposed to return int?!?!?!?! */
    *ai = (neg && num > 0) ? (qsh_ari_t)(-(qsh_ari_t)((num - 1U) & 0x7FFFFFFF) - 1)
                           : (qsh_ari_t)num;
    return (1);
}

int
getpnh(const char **sp, qiHUGE_U *ai)
{
    char c;
    const char *s;
    qiHUGE_U num = 0;
    kby state = 0;
    bool neg = false;

    s = *sp;

    do {
        c = *s++;
    } while (ctype(c, C_SPACE));

    switch (c) {
    case '-':
        neg = true;
        /* FALLTHROUGH */
    case '+':
        c = *s++;
        break;
    }

    while (ctype(c, C_DIGIT)) {
        if (num > (qiHUGE_U_MAX / 10U)) {
            /* overflow on multiplication */
            state = 2;
            errno = EOVERFLOW;
        }
        num *= 10U;
        if (num > (qiHUGE_U_MAX - (unsigned int)qsh_numdig(c))) {
            /* overflow on addition */
            state = 2;
            errno = EOVERFLOW;
        }
        if (state < 2) {
            num += (unsigned int)qsh_numdig(c);
            state = 1;
        }
        c = *s++;
    }
    --s;

    if (state)
        *sp = s;
    else
        errno = EINVAL;
    if (state != 1) {
        *ai = 0;
        return (0);
    }
    *ai = neg ? (qiHUGE_U) - (qiHUGE_U)num : num;
    return (1);
}

/**
 * pattern simplifications:
 * - @(x) -> x (not @(x|y) though)
 * - ** -> *
 */
static void *
simplify_gmatch_pattern(const unsigned char *sp)
{
    kby c;
    unsigned char *cp, *dp;
    const unsigned char *ps, *se;

    cp = alloc(strlen((const void *)sp) + 1, ATEMP);
    goto simplify_gmatch_pat1a;

    /* foo@(b@(a)r)b@(a|a)z -> foobarb@(a|a)z */
simplify_gmatch_pat1:
    sp = cp;
simplify_gmatch_pat1a:
    dp = cp;
    se = strnul(sp);
    while ((c = *sp++)) {
        if (!ISMAGIC(c)) {
            *dp++ = c;
            continue;
        }
        switch (ord((c = *sp++))) {
        case ORD(0x80 | '@'):
        /* simile for @ */
        case ORD(0x80 | ' '):
            /* check whether it has only one clause */
            ps = pat_scan(sp, se, true);
            if (!ps || ps[-1] != /*(*/ ')')
                /* nope */
                break;
            /* copy inner clause until matching close */
            ps -= 2;
            while ((const unsigned char *)sp < ps)
                *dp++ = *sp++;
            /* skip MAGIC and closing parenthesis */
            sp += 2;
            /* copy the rest of the pattern */
            memmove(dp, sp, strlen((const void *)sp) + 1);
            /* redo from start */
            goto simplify_gmatch_pat1;
        }
        *dp++ = MAGIC;
        *dp++ = c;
    }
    *dp = '\0';

    /* collapse adjacent asterisk wildcards */
    sp = dp = cp;
    while ((c = *sp++)) {
        if (!ISMAGIC(c)) {
            *dp++ = c;
            continue;
        }
        switch ((c = *sp++)) {
        case '*':
            while (ISMAGIC(sp[0]) && sp[1] == c)
                sp += 2;
            break;
        }
        *dp++ = MAGIC;
        *dp++ = c;
    }
    *dp = '\0';

    /* return the result, allocated from ATEMP */
    return (cp);
}

/* -------- gmatch.c -------- */

/*
 * int gmatch(string, pattern)
 * char *string, *pattern;
 *
 * Match a pattern as in sh(1).
 * pattern character are prefixed with MAGIC by expand.
 */
int
gmatchx(const char *s, const char *p, bool isfile)
{
    const char *se, *pe;
    char *pnew;
    int rv;

    if (s == NULL || p == NULL)
        return (0);

    pe = strnul(p);
    /*
     * isfile is false iff no syntax check has been done on the pattern.
     * If check fails just do a strcmp().
     */
    if (!isfile && !has_globbing(p)) {
        size_t len = pe - p + 1;
        char tbuf[64];
        char *t = len <= sizeof(tbuf) ? tbuf : alloc(len, ATEMP);
        debunk(t, p, len);
        return (!strcmp(t, s));
    }
    se = strnul(s);

    /*
     * since the do_gmatch() engine sucks so much, we must do some
     * pattern simplifications
     */
    pnew = simplify_gmatch_pattern((const unsigned char *)p);
    pe = strnul(pnew);

    rv = do_gmatch((const unsigned char *)s, (const unsigned char *)se, (const unsigned char *)pnew,
                   (const unsigned char *)pe, (const unsigned char *)s);
    afree(pnew, ATEMP);
    return (rv);
}

/**
 * Returns if p is a syntacticly correct globbing pattern, false if it
 * contains no pattern characters or if there is a syntax error.
 * Syntax errors are:
 *  - [ with no closing ]
 *  - imbalanced $(...) expression
 *  - [...] and *(...) not nested (eg, @(a[b|)c], *(a[b|c]d))
 */
/*XXX
 * - if no magic,
 *  if dest given, copy to dst
 *  return ?
 * - if magic && (no globbing || syntax error)
 *  debunk to dst
 *  return ?
 * - return ?
 */
bool
has_globbing(const char *pat)
{
    unsigned char c, subc;
    bool saw_glob = false;
    unsigned int nest = 0;
    const unsigned char *p = (const unsigned char *)pat;
    const unsigned char *s;

    while ((c = *p++)) {
        /* regular character? ok. */
        if (!ISMAGIC(c))
            continue;
        /* MAGIC + NUL? abort. */
        if (!(c = *p++))
            return (false);
        /* some specials */
        if (ord(c) == ORD('*') || ord(c) == ORD('?')) {
            /* easy glob, accept */
            saw_glob = true;
        } else if (ord(c) == ORD('[')) {
            /* bracket expression; eat negation and initial ] */
            if (ISMAGIC(p[0]) && ord(p[1]) == ORD('!'))
                p += 2;
            if (ISMAGIC(p[0]) && ord(p[1]) == ORD(']'))
                p += 2;
            /* check next string part */
            s = p;
            while ((c = *s++)) {
                /* regular chars are ok */
                if (!ISMAGIC(c))
                    continue;
                /* MAGIC + NUL cannot happen */
                if (!(c = *s++))
                    return (false);
                /* terminating bracket? */
                if (ord(c) == ORD(']')) {
                    /* accept and continue */
                    p = s;
                    saw_glob = true;
                    break;
                }
                /* sub-bracket expressions */
                if (ord(c) == ORD('[') && (
                                              /* collating element? */
                                              ord(*s) == ORD('.') ||
                                              /* equivalence class? */
                                              ord(*s) == ORD('=') ||
                                              /* character class? */
                                              ord(*s) == ORD(':'))) {
                    /* must stop with exactly the same c */
                    subc = *s++;
                    /* arbitrarily many chars in betwixt */
                    while ((c = *s++))
                        /* but only this sequence... */
                        if (c == subc && ISMAGIC(*s) && ord(s[1]) == ORD(']')) {
                            /* accept, terminate */
                            s += 2;
                            break;
                        }
                    /* EOS without: reject bracket expr */
                    if (!c)
                        break;
                    /* continue; */
                }
                /* anything else just goes on */
            }
        } else if ((c & 0x80) && ctype(c & 0x7F, C_PATMO | C_SPC)) {
            /* opening pattern */
            saw_glob = true;
            ++nest;
        } else if (ord(c) == ORD(/*(*/ ')')) {
            /* closing pattern */
            if (nest)
                --nest;
        }
    }
    return (saw_glob && !nest);
}

/* Function must return either 0 or 1 (assumed by code for 0x80|'!') */
static int
do_gmatch(const unsigned char *s, const unsigned char *se, const unsigned char *p,
          const unsigned char *pe, const unsigned char *smin)
{
    unsigned char sc, pc, sl = 0;
    const unsigned char *prest, *psub, *pnext;
    const unsigned char *srest;

    if (s == NULL || p == NULL)
        return (0);
    if (s > smin && s <= se)
        sl = s[-1];
    while (p < pe) {
        pc = *p++;
        sc = s < se ? *s : '\0';
        s++;
        if (!ISMAGIC(pc)) {
            if (sc != pc)
                return (0);
            sl = sc;
            continue;
        }
        switch (ord(*p++)) {
        case ORD('['):
            /* BSD cclass extension? */
            if (ISMAGIC(p[0]) && ord(p[1]) == ORD('[') && ord(p[2]) == ORD(':') &&
                ctype((pc = p[3]), C_ANGLE) && ord(p[4]) == ORD(':') && ISMAGIC(p[5]) &&
                ord(p[6]) == ORD(']') && ISMAGIC(p[7]) && ord(p[8]) == ORD(']')) {
                /* zero-length match */
                --s;
                p += 9;
                /* word begin? */
                if (ord(pc) == ORD('<') && !ctype(sl, C_ALNUX) && ctype(sc, C_ALNUX))
                    break;
                /* word end? */
                if (ord(pc) == ORD('>') && ctype(sl, C_ALNUX) && !ctype(sc, C_ALNUX))
                    break;
                /* neither */
                return (0);
            }
            if (sc == 0 || (p = gmatch_cclass(p, sc)) == NULL)
                return (0);
            break;

        case ORD('?'):
            if (sc == 0)
                return (0);
            if (UTFMODE) {
                --s;
                s += ez_mbtoc(NULL, (const void *)s);
            }
            break;

        case ORD('*'):
            if (p == pe)
                return (1);
            s--;
            do {
                if (do_gmatch(s, se, p, pe, smin))
                    return (1);
            } while (s++ < se);
            return (0);

        /**
         * [+*?@!](pattern|pattern|..)
         * This is also needed for ${..%..}, etc.
         */

        /* matches one or more times */
        case ORD('+') | 0x80:
        /* matches zero or more times */
        case ORD('*') | 0x80:
            if (!(prest = pat_scan(p, pe, false)))
                return (0);
            s--;
            /* take care of zero matches */
            if (ord(p[-1]) == (0x80 | ORD('*')) && do_gmatch(s, se, prest, pe, smin))
                return (1);
            for (psub = p;; psub = pnext) {
                pnext = pat_scan(psub, pe, true);
                for (srest = s; srest <= se; srest++) {
                    if (do_gmatch(s, srest, psub, pnext - 2, smin) &&
                        (do_gmatch(srest, se, prest, pe, smin) ||
                         (s != srest && do_gmatch(srest, se, p - 2, pe, smin))))
                        return (1);
                }
                if (pnext == prest)
                    break;
            }
            return (0);

        /* matches zero or once */
        case ORD('?') | 0x80:
        /* matches one of the patterns */
        case ORD('@') | 0x80:
        /* simile for @ */
        case ORD(' ') | 0x80:
            if (!(prest = pat_scan(p, pe, false)))
                return (0);
            s--;
            /* Take care of zero matches */
            if (ord(p[-1]) == (0x80 | ORD('?')) && do_gmatch(s, se, prest, pe, smin))
                return (1);
            for (psub = p;; psub = pnext) {
                pnext = pat_scan(psub, pe, true);
                srest = prest == pe ? se : s;
                for (; srest <= se; srest++) {
                    if (do_gmatch(s, srest, psub, pnext - 2, smin) &&
                        do_gmatch(srest, se, prest, pe, smin))
                        return (1);
                }
                if (pnext == prest)
                    break;
            }
            return (0);

        /* matches none of the patterns */
        case ORD('!') | 0x80:
            if (!(prest = pat_scan(p, pe, false)))
                return (0);
            s--;
            for (srest = s; srest <= se; srest++) {
                int matched = 0;

                for (psub = p;; psub = pnext) {
                    pnext = pat_scan(psub, pe, true);
                    if (do_gmatch(s, srest, psub, pnext - 2, smin)) {
                        matched = 1;
                        break;
                    }
                    if (pnext == prest)
                        break;
                }
                if (!matched && do_gmatch(srest, se, prest, pe, smin))
                    return (1);
            }
            return (0);

        default:
            if (sc != p[-1])
                return (0);
            break;
        }
        sl = sc;
    }
    return (s == se);
}

/*XXX this is a prime example for bsearch or a const hashtable */
static const struct cclass {
    const char *name;
    kui value;
} cclasses[] = {
    /* POSIX */
    {"alnum", C_ALNUM},
    {"alpha", C_ALPHA},
    {"blank", C_BLANK},
    {"cntrl", C_CNTRL},
    {"digit", C_DIGIT},
    {"graph", C_GRAPH},
    {"lower", C_LOWER},
    {"print", C_PRINT},
    {"punct", C_PUNCT},
    {"space", C_SPACE},
    {"upper", C_UPPER},
    {"xdigit", C_SEDEC},
    /* BSD */
    /* "<" and ">" are handled inline */
    /* GNU bash */
    {"ascii", C_ASCII},
    {"word", C_ALNUX},
    /* mksh */
    {"sh_alias", C_ALIAS},
    {"sh_edq", C_EDQ},
    {"sh_ifs", C_IFS},
    {"sh_ifsws", C_IFSWS},
    {"sh_nl", C_NL},
    {"sh_quote", C_QUOTE},
    /* sentinel */
    {NULL, 0}};

static const unsigned char *
gmatch_cclass(const unsigned char *pat, unsigned char sc)
{
    unsigned char c, subc, lc;
    const unsigned char *p = pat, *s;
    bool found = false;
    bool negated = false;
    char *subp;

    /* check for negation */
    if (ISMAGIC(p[0]) && ord(p[1]) == ORD('!')) {
        p += 2;
        negated = true;
    }
    /* make initial ] non-MAGIC */
    if (ISMAGIC(p[0]) && ord(p[1]) == ORD(']'))
        ++p;
    /* iterate over bracket expression, debunk()ing on the fly */
    while ((c = *p++)) {
    nextc:
        /* non-regular character? */
        if (ISMAGIC(c)) {
            /* MAGIC + NUL cannot happen */
            if (!(c = *p++))
                break;
            /* terminating bracket? */
            if (ord(c) == ORD(']')) {
                /* accept and return */
                return (found != negated ? p : NULL);
            }
            /* sub-bracket expressions */
            if (ord(c) == ORD('[') && (
                                          /* collating element? */
                                          ord(*p) == ORD('.') ||
                                          /* equivalence class? */
                                          ord(*p) == ORD('=') ||
                                          /* character class? */
                                          ord(*p) == ORD(':'))) {
                /* must stop with exactly the same c */
                subc = *p++;
                /* save away start of substring */
                s = p;
                /* arbitrarily many chars in betwixt */
                while ((c = *p++))
                    /* but only this sequence... */
                    if (c == subc && ISMAGIC(*p) && ord(p[1]) == ORD(']')) {
                        /* accept, terminate */
                        p += 2;
                        break;
                    }
                /* EOS without: reject bracket expr */
                if (!c)
                    break;
                /* debunk substring */
                strndupx(subp, s, p - s - 3, ATEMP);
                debunk(subp, subp, p - s - 3 + 1);
            cclass_common:
                /* whither subexpression */
                if (ord(subc) == ORD(':')) {
                    const struct cclass *cls = cclasses;

                    /* search for name in cclass list */
                    while (cls->name)
                        if (!strcmp(subp, cls->name)) {
                            /* found, match? */
                            if (ctype(sc, cls->value))
                                found = true;
                            /* break either way */
                            break;
                        } else
                            ++cls;
                    /* that's all here */
                    afree(subp, ATEMP);
                    continue;
                }
                /* collating element or equivalence class */
                /* Note: latter are treated as former */
                if (ctype(subp[0], C_ASCII) && !subp[1])
                    /* [.a.] where a is one ASCII char */
                    c = subp[0];
                else
                    /* force no match */
                    c = 0;
                /* no longer needed */
                afree(subp, ATEMP);
            } else if (!ISMAGIC(c) && (c & 0x80)) {
                /* 0x80|' ' is plain (...) */
                if ((c &= 0x7F) != ' ') {
                    /* check single match NOW */
                    if (sc == c)
                        found = true;
                    /* next character is (...) */
                }
                c = '(' /*)*/;
            }
        }
        /* range expression? */
        if (!(ISMAGIC(p[0]) && ord(p[1]) == ORD('-') &&
              /* not terminating bracket? */
              (!ISMAGIC(p[2]) || ord(p[3]) != ORD(']')))) {
            /* no, check single match */
            if (sc == c)
                /* note: sc is never NUL */
                found = true;
            /* do the next "first" character */
            continue;
        }
        /* save lower range bound */
        lc = c;
        /* skip over the range operator */
        p += 2;
        /* do the same shit as above... almost */
        subc = 0;
        if (!(c = *p++))
            break;
        /* non-regular character? */
        if (ISMAGIC(c)) {
            /* MAGIC + NUL cannot happen */
            if (!(c = *p++))
                break;
            /* sub-bracket expressions */
            if (ord(c) == ORD('[') && (
                                          /* collating element? */
                                          ord(*p) == ORD('.') ||
                                          /* equivalence class? */
                                          ord(*p) == ORD('=') ||
                                          /* character class? */
                                          ord(*p) == ORD(':'))) {
                /* must stop with exactly the same c */
                subc = *p++;
                /* save away start of substring */
                s = p;
                /* arbitrarily many chars in betwixt */
                while ((c = *p++))
                    /* but only this sequence... */
                    if (c == subc && ISMAGIC(*p) && ord(p[1]) == ORD(']')) {
                        /* accept, terminate */
                        p += 2;
                        break;
                    }
                /* EOS without: reject bracket expr */
                if (!c)
                    break;
                /* debunk substring */
                strndupx(subp, s, p - s - 3, ATEMP);
                debunk(subp, subp, p - s - 3 + 1);
                /* whither subexpression */
                if (ord(subc) == ORD(':')) {
                    /* oops, not a range */

                    /* match single previous char */
                    if (lc && (sc == lc))
                        found = true;
                    /* match hyphen-minus */
                    if (ord(sc) == ORD('-'))
                        found = true;
                    /* handle cclass common part */
                    goto cclass_common;
                }
                /* collating element or equivalence class */
                /* Note: latter are treated as former */
                if (ctype(subp[0], C_ASCII) && !subp[1])
                    /* [.a.] where a is one ASCII char */
                    c = subp[0];
                else
                    /* force no match */
                    c = 0;
                /* no longer needed */
                afree(subp, ATEMP);
                /* other meaning below */
                subc = 0;
            } else if (c == (0x80 | ' ')) {
                /* 0x80|' ' is plain (...) */
                c = '(' /*)*/;
            } else if (!ISMAGIC(c) && (c & 0x80)) {
                c &= 0x7F;
                subc = '(' /*)*/;
            }
        }
        /* now do the actual range match check */
        if (lc != 0 /* && c != 0 */ && asciibetical(lc) <= asciibetical(sc) &&
            asciibetical(sc) <= asciibetical(c))
            found = true;
        /* forced next character? */
        if (subc) {
            c = subc;
            goto nextc;
        }
        /* otherwise, just go on with the pattern string */
    }
    /* if we broke here, the bracket expression was invalid */
    if (ord(sc) == ORD('['))
        /* initial opening bracket as literal match */
        return (pat);
    /* or rather no match */
    return (NULL);
}

/* Look for next ) or | (if match_sep) in *(foo|bar) pattern */
static const unsigned char *
pat_scan(const unsigned char *p, const unsigned char *pe, bool match_sep)
{
    int nest = 0;

    for (; p < pe; p++) {
        if (!ISMAGIC(*p))
            continue;
        if ((*++p == /*(*/ ')' && nest-- == 0) || (*p == '|' && match_sep && nest == 0))
            return (p + 1);
        if ((*p & 0x80) && ctype(*p & 0x7F, C_PATMO | C_SPC))
            nest++;
    }
    return (NULL);
}

