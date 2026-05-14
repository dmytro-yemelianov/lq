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
#include "var_priv.h"


int
special(const char *name)
{
    struct tbl *tp;

    tp = ktsearch(&specials, name, hash(name));
    return (tp && (tp->flag & ISSET) ? tp->type : V_NONE);
}

/* Make a variable non-special */
static void
unspecial(const char *name)
{
    struct tbl *tp;

    tp = ktsearch(&specials, name, hash(name));
    if (tp)
        ktdelete(tp);
}

static time_t seconds;          /* time SECONDS last set */
static qsh_uari_t user_lineno; /* what user set $LINENO to */

/* minimum values from the OS we consider sane, lowered for R53 */
#define MIN_COLS 4
#define MIN_LINS 2

void
getspec(struct tbl *vp)
{
    qsh_ari_u num;
    int st;
    struct timeval tv;

    switch ((st = special(vp->name))) {
    case V_COLUMNS:
    case V_LINES:
        /*
         * Do NOT export COLUMNS/LINES. Many applications
         * check COLUMNS/LINES before checking ws.ws_col/row,
         * so if the app is started with C/L in the environ
         * and the window is then resized, the app won't
         * see the change cause the environ doesn't change.
         */
        if (got_winch)
            change_winsz();
        break;
    }
    switch (st) {
    case V_BASHPID:
        num.u = (qsh_uari_t)procpid;
        break;
    case V_COLUMNS:
        num.i = x_cols;
        break;
    case V_HISTSIZE:
        num.i = histsize;
        break;
    case V_LINENO:
        num.u = (qsh_uari_t)current_lineno + user_lineno;
        break;
    case V_LINES:
        num.i = x_lins;
        break;
    case V_EPOCHREALTIME: {
        /* 10(%u) + 1(.) + 6 + NUL */
        char buf[18];

        vp->flag &= ~SPECIAL;
        qsh_TIME(tv);
        if (vp->flag & INTEGER)
            setint(vp, (qsh_ari_t)tv.tv_sec);
        else {
            shf_snprintf(buf, sizeof(buf), "%u.%06u", (unsigned)tv.tv_sec, (unsigned)tv.tv_usec);
            setstr(vp, buf, QSH_RETURN_ERROR | 0x4);
        }
        vp->flag |= SPECIAL;
        return;
    }
    case V_OPTIND:
        num.i = user_opt.uoptind;
        break;
    case V_RANDOM:
        num.i = rndget();
        break;
    case V_SECONDS:
        /*
         * On start up the value of SECONDS is used before
         * it has been set - don't do anything in this case
         * (see initcoms[] in main.c).
         */
        if (vp->flag & ISSET) {
            qsh_TIME(tv);
            num.i = tv.tv_sec - seconds;
        } else
            return;
        break;
    default:
        /* do nothing, do not touch vp at all */
        return;
    }
    vp->flag &= ~SPECIAL;
    setint_n(vp, num.i, 0);
    vp->flag |= SPECIAL;
}

void
setspec(struct tbl *vp)
{
    qsh_ari_u num;
    char *s;
    int st = special(vp->name);

    switch (st) {
    case V_IFS:
        set_ifs(str_val(vp));
        return;
    case V_PATH:
        afree(path, APERM);
        s = str_val(vp);
        strdupx(path, s, APERM);
        /* clear tracked aliases */
        flushcom(true);
        return;
    case V_TERM:
        x_initterm(str_val(vp));
        return;
    case V_TMPDIR:
        afree(tmpdir, APERM);
        tmpdir = NULL;
        /*
         * Use tmpdir iff it is an absolute path, is writable
         * and searchable and is a directory...
         */
        {
            struct stat statb;

            s = str_val(vp);
            /* LINTED use of access */
            if (qsh_abspath(s) && access(s, W_OK | X_OK) == 0 && stat(s, &statb) == 0 &&
                S_ISDIR(statb.st_mode))
                strdupx(tmpdir, s, APERM);
        }
        return;
    /* common sub-cases */
    case V_COLUMNS:
    case V_LINES:
        if (vp->flag & IMPORT) {
            /* do not touch */
            unspecial(vp->name);
            vp->flag &= ~SPECIAL;
            return;
        }
        /* FALLTHROUGH */
    case V_HISTSIZE:
    case V_LINENO:
    case V_OPTIND:
    case V_RANDOM:
    case V_SECONDS:
    case V_TMOUT:
        vp->flag &= ~SPECIAL;
        if (getint(vp, &num, false) == -1) {
            s = str_val(vp);
            if (st != V_RANDOM)
                kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_THREEMSG | KWF_NOERRNO, vp->name,
                      Tbadnum, s);
            num.u = hash(s);
        }
        vp->flag |= SPECIAL;
        break;
    default:
        /* do nothing, do not touch vp at all */
        return;
    }

    /* process the singular parts of the common cases */

    switch (st) {
    case V_COLUMNS:
        if (num.i >= MIN_COLS)
            x_cols = num.i;
        break;
    case V_HISTSIZE:
        sethistsize(num.i);
        break;
    case V_LINENO:
        /* The -1 is because line numbering starts at 1. */
        user_lineno = num.u - (qsh_uari_t)current_lineno - 1;
        break;
    case V_LINES:
        if (num.i >= MIN_LINS)
            x_lins = num.i;
        break;
    case V_OPTIND:
        getopts_reset((int)num.i);
        break;
    case V_RANDOM:
        /*
         * mksh R39d+ no longer has the traditional repeatability
         * of $RANDOM sequences, but always retains state
         */
        rndset((unsigned long)num.u);
        break;
    case V_SECONDS: {
        struct timeval tv;

        qsh_TIME(tv);
        seconds = tv.tv_sec - num.i;
    } break;
    case V_TMOUT:
        qsh_tmout = num.i >= 0 ? num.i : 0;
        break;
    }
}

void
unsetspec(struct tbl *vp, bool dounset)
{
    /*
     * AT&T ksh man page says OPTIND, OPTARG and _ lose special
     * meaning, but OPTARG does not (still set by getopts) and _ is
     * also still set in various places. Don't know what AT&T does
     * for HISTSIZE, HISTFILE. Unsetting these in AT&T ksh does not
     * loose the 'specialness': IFS, COLUMNS, PATH, TMPDIR
     */

    switch (special(vp->name)) {
    case V_IFS:
        set_ifs(TC_IFSWS);
        return;
    case V_PATH:
        afree(path, APERM);
        strdupx(path, def_path, APERM);
        /* clear tracked aliases */
        flushcom(true);
        return;
    case V_TERM:
        x_initterm(null);
        return;
    case V_TMPDIR:
        /* should not become unspecial */
        if (tmpdir) {
            afree(tmpdir, APERM);
            tmpdir = NULL;
        }
        return;
    case V_LINENO:
    case V_RANDOM:
    case V_SECONDS:
    case V_TMOUT:
        /* AT&T ksh leaves previous value in place */
        unspecial(vp->name);
        return;
    /* should not become unspecial, but allow unsetting */
    case V_COLUMNS:
    case V_LINES:
        if (dounset)
            unspecial(vp->name);
        return;
    }
}

/*
 * Search for (and possibly create) a table entry starting with
 * vp, indexed by val.
 */
struct tbl *
arraysearch(struct tbl *vp, k32 idx)
{
    struct tbl *prev, *curr, *news;
    size_t len;

    vp->flag = (vp->flag | (ARRAY | DEFINED)) & ~ASSOC;
    /* the table entry is always [0] */
    if (idx == 0)
        return (vp);
    prev = vp;
    curr = vp->u.array;
    while (curr && curr->ua.index < idx) {
        prev = curr;
        curr = curr->u.array;
    }
    if (curr && curr->ua.index == idx) {
        if (curr->flag & ISSET)
            return (curr);
        news = curr;
    } else
        news = NULL;
    if (!news) {
        len = strlen(vp->name) + 1U;
        news = alloc(qccFAMsz(struct tbl, name, len), vp->areap);
        memcpy(news->name, vp->name, len);
    }
    news->flag = (vp->flag & ~(ALLOC | DEFINED | ISSET | SPECIAL)) | AINDEX;
    news->type = vp->type;
    news->areap = vp->areap;
    news->u2.field = vp->u2.field;
    news->ua.index = idx;

    if (curr != news) {
        /* not reusing old array entry */
        prev->u.array = news;
        news->u.array = curr;
    }
    return (news);
}

/*
 * Return the length of an array reference (eg, [1+2]) - cp is assumed
 * to point to the open bracket. Returns 0 if there is no matching
 * closing bracket.
 *
 * XXX this should parse the actual arithmetic syntax
 */
size_t
array_ref_len(const char *cp)
{
    const char *s = cp;
    char c;
    int depth = 0;

    while ((c = *s++) && (ord(c) != ORD(']') || --depth))
        if (ord(c) == ORD('['))
            depth++;
    if (!c)
        return (0);
    return (s - cp);
}

/*
 * same effect as global(copy of the base of an array name)
 */
struct tbl *
arraybase(const char *str)
{
    const char *p;
    char *s, sbuf[32];
    size_t n;
    struct tbl *rv;

    n = strlen(str);
    if ((p = memchr(str, '[', n)))
        n = p - str;
    strnbdupx(s, str, n, ATEMP, sbuf);
    rv = global(s);
    if (s != sbuf)
        afree(s, ATEMP);

    return (rv);
}

/* set (or overwrite, if reset) the array variable var to the values in vals */
qsh_uari_t
set_array(const char *var, bool reset, const char **vals)
{
    struct tbl *vp, *vq;
    qsh_uari_t i = 0, j = 0;
    const char *ccp = var;
    char *cp = NULL;
    size_t n;

    /* to get local array, use "local foo; set -A foo" */
    n = strlen(var);
    if (n > 0 && var[n - 1] == '+') {
        /* append mode */
        reset = false;
        strndupx(cp, var, n - 1, ATEMP);
        ccp = cp;
    }
    vp = global(ccp);

    /* Note: AT&T ksh allows set -A but not set +A of a read-only var */
    if ((vp->flag & RDONLY))
        kerrf(KWF_ERR(2) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, Tread_only, ccp);
    /* This code is quite non-optimal */
    if (reset) {
        /* trash existing values and attributes */
        unset(vp, 1);
        /* allocate-by-access the [0] element to keep in scope */
        arraysearch(vp, 0);
        /* honour set -o allexport */
        if (Flag(FEXPORT))
            typeset(ccp, EXPORT, 0, 0, 0);
    }
    /*
     * TODO: would be nice for assignment to completely succeed or
     * completely fail. Only really effects integer arrays:
     * evaluation of some of vals[] may fail...
     */
    if (cp != NULL) {
        /* find out where to set when appending */
        for (vq = vp; vq; vq = vq->u.array) {
            if (!(vq->flag & ISSET))
                continue;
            if (arrayindex(vq) >= j)
                j = arrayindex(vq) + 1;
        }
        afree(cp, ATEMP);
    }
    while ((ccp = vals[i])) {

        vq = arraysearch(vp, qiMM(k32, K32_FM, j));
        /* would be nice to deal with errors here... (see above) */
        setstr(vq, ccp, QSH_RETURN_ERROR);
        i++;
        j++;
    }

    return (i);
}

void
change_winsz(void)
{
    /* devc-ser8250 doesn't report window size; pin defaults.
     * When a richer terminal driver lands, this will query it (no
     * TIOCGWINSZ ioctl involved). */
    if (x_cols < MIN_COLS)
        x_cols = 80;
    if (x_lins < MIN_LINS)
        x_lins = 24;
}

k32
hash(const void *s)
{
    register k32 h;

    BAFHInit(h);
    BAFHUpdateStr(h, s);
    BAFHFinish(h);
    return (h);
}

void
chvt_rndsetup(const void *bp, size_t sz)
{
    register k32 h;

    h = lcg_state;
    BAFHUpdateVLQ(h, size_t, sz);
    BAFHFinish(h);
    /* variation through pid, ppid, and the works */
    BAFHUpdateMem(h, &rndsetupstate, sizeof(rndsetupstate));
    /* some variation, some possibly entropy, depending on OE */
    BAFHUpdateMem(h, bp, sz);
    /* mix them all up */
    BAFHFinish(h);
    lcg_state = h;
}

k32
rndget(void)
{
    /*
     * this is the same Linear Congruential PRNG as Borland
     * C/C++ allegedly uses in its built-in rand() function
     */
    lcg_state = qiMO(k32, K32_FM, qiMO(k32, K32_FM, 22695477U, *, lcg_state), +, 1U);
    return (qiMO(k32, K32_FM, qiMKshr(k32, K32_FM, lcg_state, 16), &, 0x7FFFU));
}

#if HAVE_GETRANDOM || defined(arc4random_pushb_fast) || defined(QSH_A4PB)
#define QSH_USE_ARC4RANDOM 0
#else
#define QSH_USE_ARC4RANDOM 0
#endif

void
rndset(unsigned long v)
{
    register k32 h;
#if defined(arc4random_pushb_fast) || defined(QSH_A4PB)
    register k32 t;
#endif
    struct {
        struct timeval tv;
        void *sp;
        k32 qh;
        pid_t pp;
        unsigned short r;
    } z;

    /* clear the allocated space, for valgrind and to avoid UB */
    memset(&z, 0, sizeof(z));

    h = lcg_state ? lcg_state : (k32)1U;
    BAFHFinish(h);
    BAFHUpdateMem(h, &v, sizeof(v));

    qsh_TIME(z.tv);
    z.sp = &z;
    z.pp = procpid;
    z.r = rndget();
    /* nōn-blocking extra bytes from OS… if cheap and available */

#if defined(arc4random_pushb_fast) || defined(QSH_A4PB)
    z.qh = (qh_state & 0xFFFF8000U) | rndget();
    lcg_state = qiMKshl(k32, K32_FM, qh_state, 15) | rndget();
    /*
     * either we have very chap entropy get and push available,
     * with malloc() pulling in this code already anyway, or the
     * user requested us to use the old functions
     */
    t = h;
    BAFHUpdateMem(t, &lcg_state, sizeof(lcg_state));
    BAFHFinish(t);
    lcg_state = t;
#if defined(arc4random_pushb_fast)
    arc4random_pushb_fast(&lcg_state, sizeof(lcg_state));
    lcg_state = arc4random();
#else
    lcg_state = arc4random_pushb(&lcg_state, sizeof(lcg_state));
#endif
    BAFHUpdateMem(h, &lcg_state, sizeof(lcg_state));
#else
    z.qh = qh_state;
#endif

    BAFHUpdateMem(h, &z, sizeof(z));
    BAFHFinish(h);
    lcg_state = h;
}

void
rndpush(const void *s, size_t n)
{
    register k32 h = qh_state ? qh_state : (k32)1U;

    BAFHUpdateMem(h, s, n);
    BAFHFinish(h);
    qh_state = h;
}

/* record last glob match */
void
record_match(const char *istr)
{
    struct tbl *vp;

    vp = local("QSH_MATCH", false);
    unset(vp, 1);
    vp->flag = DEFINED | RDONLY;
    setstr(vp, istr, 0x4);
}

/* typeset, export and readonly */
int
c_typeset(const char **wp)
{
    struct tbl *vp, **p;
    kui fset = 0, fclr = 0, flag;
    int thing = 0, field = 0, base = 0, i;
    struct block *l;
    const char *opts;
    const char *fieldstr = NULL, *basestr = NULL;
    bool localv = false, func = false, pflag = false, istset = true;
    enum namerefflag new_refflag = SRF_NOP;

    switch (**wp) {

    /* export */
    case 'e':
        fset |= EXPORT;
        istset = false;
        break;

    /* readonly */
    case 'r':
        fset |= RDONLY;
        istset = false;
        break;

    /* set */
    case 's':
        /* called with 'typeset -' */
        break;

    /* typeset */
    case 't':
        localv = true;
        break;
    }

    /* see comment below regarding possible options */
    opts = istset ? "L#R#UZ#afgi#lnprtux" : "p";

    builtin_opt.flags |= GF_PLUSOPT;
    /*
     * AT&T ksh seems to have 0-9 as options which are multiplied
     * to get a number that is used with -L, -R, -Z or -i (eg, -1R2
     * sets right justify in a field of 12). This allows options
     * to be grouped in an order (eg, -Lu12), but disallows -i8 -L3 and
     * does not allow the number to be specified as a separate argument
     * Here, the number must follow the RLZi option, but is optional
     * (see the # kludge in qsh_getopt()).
     */
    while ((i = qsh_getopt(wp, &builtin_opt, opts)) != -1) {
        flag = 0;
        switch (i) {
        case 'L':
            flag = LJUST;
            fieldstr = builtin_opt.optarg;
            break;
        case 'R':
            flag = RJUST;
            fieldstr = builtin_opt.optarg;
            break;
        case 'U':
            /*
             * AT&T ksh uses u, but this conflicts with
             * upper/lower case. If this option is changed,
             * need to change the -U below as well
             */
            flag = INT_U;
            break;
        case 'Z':
            flag = ZEROFIL;
            fieldstr = builtin_opt.optarg;
            break;
        case 'a':
            /*
             * this is supposed to set (-a) or unset (+a) the
             * indexed array attribute; it does nothing on an
             * existing regular string or indexed array though
             */
            break;
        case 'f':
            func = true;
            break;
        case 'g':
            localv = ((bool)(builtin_opt.info & GI_PLUS));
            break;
        case 'i':
            flag = INTEGER;
            basestr = builtin_opt.optarg;
            break;
        case 'l':
            flag = LCASEV;
            break;
        case 'n':
            new_refflag = (builtin_opt.info & GI_PLUS) ? SRF_DISABLE : SRF_ENABLE;
            break;
        /* export, readonly: POSIX -p flag */
        case 'p':
            /* typeset: show values as well */
            pflag = true;
            if (istset)
                continue;
            break;
        case 'r':
            flag = RDONLY;
            break;
        case 't':
            flag = TRACE;
            break;
        case 'u':
            /* upper case / autoload */
            flag = UCASEV_AL;
            break;
        case 'x':
            flag = EXPORT;
            break;
        case '?':
            return (1);
        }
        if (builtin_opt.info & GI_PLUS) {
            fclr |= flag;
            fset &= ~flag;
            thing = '+';
        } else {
            fset |= flag;
            fclr &= ~flag;
            thing = '-';
        }
    }

    if (fieldstr && !getn(fieldstr, &field)) {
        kwarnf(KWF_BIERR | KWF_TWOMSG, Tbadnum, fieldstr);
        return (1);
    }
    if (basestr) {
        if (!getn(basestr, &base)) {
            kwarnf(KWF_BIERR | KWF_TWOMSG, "bad integer base", basestr);
            return (1);
        }
        if (base < 1 || base > 36)
            base = 10;
    }

    if (!(builtin_opt.info & GI_MINUSMINUS) && wp[builtin_opt.optind] &&
        (wp[builtin_opt.optind][0] == '-' || wp[builtin_opt.optind][0] == '+') &&
        wp[builtin_opt.optind][1] == '\0') {
        thing = wp[builtin_opt.optind][0];
        builtin_opt.optind++;
    }

    if (func && (((fset | fclr) & ~(TRACE | UCASEV_AL | EXPORT)) || new_refflag != SRF_NOP)) {
        kwarnf(KWF_BIERR | KWF_ONEMSG | KWF_NOERRNO,
               "only -t, -u and -x options may be used with -f");
        return (1);
    }
    if (wp[builtin_opt.optind]) {
        /*
         * Take care of exclusions.
         * At this point, flags in fset are cleared in fclr and vice
         * versa. This property should be preserved.
         */
        if (fset & LCASEV)
            /* LCASEV has priority over UCASEV_AL */
            fset &= ~UCASEV_AL;
        if (fset & LJUST)
            /* LJUST has priority over RJUST */
            fset &= ~RJUST;
        if ((fset & (ZEROFIL | LJUST)) == ZEROFIL) {
            /* -Z implies -ZR */
            fset |= RJUST;
            fclr &= ~RJUST;
        }
        /*
         * Setting these attributes clears the others, unless they
         * are also set in this command
         */
        if ((fset & (LJUST | RJUST | ZEROFIL | UCASEV_AL | LCASEV | INTEGER | INT_U | INT_L)) ||
            new_refflag != SRF_NOP)
            fclr |=
                ~fset & (LJUST | RJUST | ZEROFIL | UCASEV_AL | LCASEV | INTEGER | INT_U | INT_L);
    }
    if (new_refflag != SRF_NOP) {
        fclr &= ~(ARRAY | ASSOC);
        fset &= ~(ARRAY | ASSOC);
        fclr |= EXPORT;
        fset |= ASSOC;
        if (new_refflag == SRF_DISABLE)
            fclr |= ASSOC;
    }

    /* set variables and attributes */
    if (wp[builtin_opt.optind] &&
        /* not "typeset -p varname" */
        !(!func && pflag && !(fset | fclr))) {
        int rv = 0, x;
        struct tbl *f;

        if (localv && !func)
            fset |= LOCAL;
        for (i = builtin_opt.optind; wp[i]; i++) {
            if (func) {
                f = findfunc(wp[i], hash(wp[i]), ((bool)(fset & UCASEV_AL)));
                if (!f) {
                    /* AT&T ksh does ++rv: bogus */
                    rv = 1;
                    continue;
                }
                if (fset | fclr) {
                    f->flag |= fset;
                    f->flag &= ~fclr;
                } else {
                    fpFUNCTf(shl_stdout, 0, ((bool)(f->flag & FKSH)), wp[i], f->val.t);
                    shf_putc('\n', shl_stdout);
                }
            } else if (!vtypeset(&x, wp[i], fset, fclr, field, base)) {
                if (x)
                    return (x);
                kwarnf(KWF_BIERR | KWF_TWOMSG | KWF_NOERRNO, wp[i], Tnot_ident);
                return (1);
            }
        }
        return (rv);
    }

    /* list variables and attributes */

    /* no difference at this point.. */
    flag = fset | fclr;
    if (func) {
        for (l = e->loc; l; l = l->next) {
            for (p = ktsort(&l->funs); (vp = *p++);) {
                if (flag && (vp->flag & flag) == 0)
                    continue;
                if (thing == '-')
                    fpFUNCTf(shl_stdout, 0, ((bool)(vp->flag & FKSH)), vp->name, vp->val.t);
                else
                    shf_puts(vp->name, shl_stdout);
                shf_putc('\n', shl_stdout);
            }
        }
    } else if (wp[builtin_opt.optind]) {
        for (i = builtin_opt.optind; wp[i]; i++) {
            vp = isglobal(wp[i], false);
            c_typeset_vardump(vp, flag, thing, last_lookup_was_array ? 4 : 0, pflag, istset);
        }
    } else
        c_typeset_vardump_recursive(e->loc, flag, thing, pflag, istset);
    return (0);
}

void
c_typeset_vardump_recursive(struct block *l, kui flag, int thing, bool pflag, bool istset)
{
    struct tbl **blockvars, *vp;

    if (l->next)
        c_typeset_vardump_recursive(l->next, flag, thing, pflag, istset);
    blockvars = ktsort(&l->vars);
    while ((vp = *blockvars++))
        c_typeset_vardump(vp, flag, thing, 0, pflag, istset);
    /*XXX doesn’t this leak? */
}

void
c_typeset_vardump(struct tbl *vp, kui flag, int thing, int any_set, bool pflag, bool istset)
{
    struct tbl *tvp;

    if (!vp)
        return;

    /*
     * See if the parameter is set (for arrays, if any
     * element is set).
     */
    for (tvp = vp; tvp; tvp = tvp->u.array)
        if (tvp->flag & ISSET) {
            any_set |= 1;
            break;
        }

    /*
     * Check attributes - note that all array elements
     * have (should have?) the same attributes, so checking
     * the first is sufficient.
     *
     * Report an unset param only if the user has
     * explicitly given it some attribute (like export);
     * otherwise, after "echo $FOO", we would report FOO...
     */
    if (!any_set && !(vp->flag & USERATTRIB))
        return;
    if (flag && (vp->flag & flag) == 0)
        return;
    if (!(vp->flag & ARRAY))
        /* optimise later conditionals */
        any_set = 0;
    do {
        bool baseone = false;

        /*
         * Ignore array elements that aren't set unless there
         * are no set elements, in which case the first is
         * reported on
         */
        if (any_set && !(vp->flag & ISSET))
            continue;
        /* no arguments */
        if (!thing && !flag) {
            if (any_set == 1) {
                shprintf(Tf_s_s_sN, Tset, "-A", vp->name);
                any_set = 2;
            }
            /*
             * AT&T ksh prints things like export, integer,
             * leftadj, zerofill, etc., but POSIX says must
             * be suitable for re-entry...
             */
            shprintf(Tf_s_, Ttypeset);
            if (((vp->flag & (ARRAY | ASSOC)) == ASSOC))
                shprintf(Tf__c_, 'n');
            if ((vp->flag & INTEGER)) {
                if (vp->type == 1) {
                    baseone = true;
                    shf_puts("-i1 ", shl_stdout);
                } else
                    shprintf(Tf__c_, 'i');
            }
            if ((vp->flag & EXPORT))
                shprintf(Tf__c_, 'x');
            if ((vp->flag & RDONLY))
                shprintf(Tf__c_, 'r');
            if ((vp->flag & TRACE))
                shprintf(Tf__c_, 't');
            if ((vp->flag & LJUST))
                shprintf("-L%d ", vp->u2.field);
            if ((vp->flag & RJUST))
                shprintf("-R%d ", vp->u2.field);
            if ((vp->flag & ZEROFIL))
                shprintf(Tf__c_, 'Z');
            if ((vp->flag & LCASEV))
                shprintf(Tf__c_, 'l');
            if ((vp->flag & UCASEV_AL))
                shprintf(Tf__c_, 'u');
            if ((vp->flag & INT_U))
                shprintf(Tf__c_, 'U');
        } else if (pflag) {
            shprintf(Tf_s_, istset ? Ttypeset : (flag & EXPORT) ? Texport : Treadonly);
        }
        shf_puts(vp->name, shl_stdout);
        if (any_set)
            shprintf(Tf_SQlu, arrayindex(vp));
        if (((!thing && !flag && pflag) || thing == '-') && (vp->flag & ISSET)) {
            shf_putc('=', shl_stdout);
            if (baseone)
                shprintf(vp->val.u > 0xFF ? "16#%04X" : "16#%02X", (unsigned int)vp->val.u);
            else {
                const char *s = str_val(vp);

                /* AT&T ksh can't have justified integers... */
                if (IS(vp->flag, INTEGER | LJUST | RJUST, INTEGER))
                    shf_puts(s, shl_stdout);
                else
                    print_value_quoted(shl_stdout, s);
            }
        }
        shf_putc('\n', shl_stdout);

        /*
         * Only report first 'element' of an array with
         * no set elements.
         */
        if (!any_set)
            return;
    } while (!(any_set & 4) && (vp = vp->u.array));
}
