/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/*
 * string expansion
 *
 * first pass: quoting, IFS separation, ~, ${}, $() and $(()) substitution.
 * second pass: alternation ({,}), filename expansion (*?[]).
 */

#include "eval_priv.h"

/* States used for field splitting (only expand() cares about these) */
#define IFS_WORD 0  /* word has chars (or quotes except "$@") */
#define IFS_WS 1    /* have seen IFS white-space */
#define IFS_NWS 2   /* have seen IFS non-white-space */
#define IFS_IWS 3   /* beginning of word, ignore IFS WS */
#define IFS_QUOTE 4 /* beg.w/quote, become IFS_WORD unless "$@" */

/* eval_expand.c entry points and utflen/utfincptr are declared in
 * eval_priv.h.  valsub/funsub now live in eval_expand.c. */

/* null (sh.h) */
char null_string[4] = {0, 0, 0, 0};

/* UTFMODE functions */
int
utflen(const char *s)
{
    size_t n;

    if (UTFMODE) {
        n = 0;
        while (*s) {
            s += ez_mbtoc(NULL, s);
            ++n;
        }
    } else
        n = strlen(s);

    if (n > 2147483647)
        n = 2147483647;
    return ((int)n);
}

void
utfincptr(const char *s, qsh_ari_t *lp)
{
    const char *cp = s;

    while ((*lp)--)
        cp += ez_mbtoc(NULL, cp);
    *lp = cp - s;
}

/* compile and expand word */
char *
substitute(const char *cp, int f)
{
    struct source *s, *sold;

    sold = source;
    s = pushs(SWSTR, ATEMP);
    s->start = s->str = cp;
    source = s;
    if (yylex(ONEWORD) != LWORD)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_ONEMSG | KWF_NOERRNO, Tbadsubst);
    source = sold;
    afree(s, ATEMP);
    return (evalstr(yylval.cp, f));
}

/*
 * expand arg-list
 */
char **
eval(const char **ap, int f)
{
    XPtrV w;

    if (*ap == NULL) {
        union qsh_ccphack vap;

        vap.ro = ap;
        return (vap.rw);
    }
    XPinit(w, 32);
    /* space for shell name */
    XPput(w, NULL);
    while (*ap != NULL)
        expand(*ap++, &w, f);
    XPput(w, NULL);
    return ((char **)XPclose(w) + 1);
}

/*
 * expand string
 */
char *
evalstr(const char *cp, int f)
{
    XPtrV w;
    char *dp = null;

    XPinit(w, 1);
    expand(cp, &w, f);
    if (XPsize(w))
        dp = *XPptrv(w);
    XPfree(w);
    return (dp);
}

/*
 * expand string - return only one component
 * used from iosetup to expand redirection files
 */
char *
evalonestr(const char *cp, int f)
{
    XPtrV w;
    char *rv;

    XPinit(w, 1);
    expand(cp, &w, f);
    switch (XPsize(w)) {
    case 0:
        rv = null;
        break;
    case 1:
        rv = (char *)*XPptrv(w);
        break;
    default:
        rv = evalstr(cp, f & ~DOGLOB);
        break;
    }
    XPfree(w);
    return (rv);
}

/* for nested substitution: ${var:=$var2} */
typedef struct SubType {
    struct tbl *var;      /* variable for ${var..} */
    struct SubType *prev; /* old type */
    struct SubType *next; /* poped type (to avoid re-allocating) */
    size_t base;          /* start position of expanded word */
    unsigned short stype; /* [=+-?%#] action after expanded word */
    short f;              /* saved value of f (DOPAT, etc) */
    kby quotep;           /* saved value of quote (for ${..[%#]..}) */
    kby quotew;           /* saved value of quote (for ${..[+-=]..}) */
} SubType;

void
expand(
    /* input word */
    const char *ccp,
    /* output words */
    XPtrV *wp,
    /* DO* flags */
    int f)
{
    int c = 0;
    /* expansion type */
    int type;
    /* quoted */
    int quote = 0;
    /* destination string and live pointer */
    XString ds;
    char *dp;
    /* source */
    const char *sp;
    /* second pass flags */
    int fdo;
    /* have word */
    int word;
    /* field splitting of parameter/command substitution */
    int doblank;
    /* expansion variables */
    Expand x = {NULL, {NULL}, NULL, 0};
    SubType st_head, *st;
    /* record number of trailing newlines in COMSUB */
    int newlines = 0;
    bool saw_eq, make_magic;
    unsigned int tilde_ok;
    size_t len;
    char *cp;

    if (ccp == NULL)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_ONEMSG | KWF_NOERRNO, "expand(NULL)");
    /* for alias, readonly, set, typeset commands */
    if ((f & DOVACHECK) && is_wdvarassign(ccp, false)) {
        f &= ~(DOVACHECK | DOBLANK | DOGLOB | DOTILDE);
        f |= DOASNTILDE | DOSCALAR;
    }
    if (Flag(FNOGLOB))
        f &= ~DOGLOB;
    if (Flag(FMARKDIRS))
        f |= DOMARKDIRS;
    if (Flag(FBRACEEXPAND) && (f & DOGLOB))
        f |= DOBRACE;

    /* init destination string */
    Xinit(ds, dp, 128, ATEMP);
    type = XBASE;
    sp = ccp;
    fdo = 0;
    saw_eq = false;
    /* must be 1/0 */
    tilde_ok = (f & (DOTILDE | DOASNTILDE)) ? 1 : 0;
    doblank = 0;
    make_magic = false;
    word = (f & DOBLANK) ? IFS_WS : IFS_WORD;
    /* clang doesn't know OSUBST comes before CSUBST */
    memset(&st_head, 0, sizeof(st_head));
    st = &st_head;

    while (/* CONSTCOND */ 1) {
        Xcheck(ds, dp);

        switch (type) {
        case XBASE:
            /* original prefixed string */
            c = ord(*sp++);
            switch (c) {
            case EOS:
                c = 0;
                break;
            case CHAR:
                c = ord(*sp++);
                break;
            case QCHAR:
                /* temporary quote */
                quote |= 2;
                c = ord(*sp++);
                break;
            case OQUOTE:
                if (word != IFS_WORD)
                    word = IFS_QUOTE;
                tilde_ok = 0;
                quote = 1;
                continue;
            case CQUOTE:
                if (word == IFS_QUOTE)
                    word = IFS_WORD;
                quote = st->quotew;
                continue;
            case COMASUB:
            case COMSUB:
            case FUNASUB:
            case FUNSUB:
            case VALSUB:
                tilde_ok = 0;
                if (f & DONTRUNCOMMAND) {
                    word = IFS_WORD;
                    *dp++ = '$';
                    switch (c) {
                    case COMASUB:
                    case COMSUB:
                        *dp++ = '(';
                        c = ORD(')');
                        break;
                    case FUNASUB:
                    case FUNSUB:
                    case VALSUB:
                        *dp++ = '{';
                        *dp++ = c == VALSUB ? '|' : ' ';
                        c = ORD('}');
                        break;
                    }
                    while (*sp != '\0') {
                        Xcheck(ds, dp);
                        *dp++ = *sp++;
                    }
                    if ((unsigned int)c == ORD(/*{*/ '}'))
                        *dp++ = ';';
                    *dp++ = c;
                } else {
                    type = comsub(&x, sp, c);
                    if (type != XBASE && (f & DOBLANK))
                        doblank++;
                    sp = strnul(sp) + 1;
                    newlines = 0;
                }
                continue;
            case EXPRSUB:
                tilde_ok = 0;
                if (f & DONTRUNCOMMAND) {
                    word = IFS_WORD;
                    *dp++ = '$';
                    *dp++ = '(';
                    *dp++ = '(';
                    while (*sp != '\0') {
                        Xcheck(ds, dp);
                        *dp++ = *sp++;
                    }
                    *dp++ = ')';
                    *dp++ = ')';
                } else {
                    union tbl_static v;

                    v.tbl.flag = DEFINED | ISSET | INTEGER;
                    /* not default */
                    v.tbl.type = 10;
                    v.tbl.name[0] = '\0';
                    v_evaluate((struct tbl *)&v, substitute(sp, 0), QSH_UNWIND_ERROR, true);
                    sp = strnul(sp) + 1;
                    x.str = str_val((struct tbl *)&v);
                    type = XSUB;
                    if (f & DOBLANK)
                        doblank++;
                }
                continue;
            case OSUBST: {
                /* ${{#}var{:}[=+-?#%]word} */
                /*-
             * format is:
             *  OSUBST [{x] plain-variable-part \0
             *      compiled-word-part CSUBST [}x]
             * This is where all syntax checking gets done...
             */
                /* skip the { or x (}) */
                const char *varname = ++sp;
                unsigned int stype /* for GCC */ = 0;
                int slen = 0;

                /* skip variable */
                sp = strnul(sp) + 1;
                type = varsub(&x, varname, sp, &stype, &slen);
                if (type < 0) {
                    char *beg, *end, *str;
                unwind_substsyn:
                    /* restore sp */
                    sp = varname - 2;
                    beg = wdcopy(sp, ATEMP);
                    end = (wdscan(strnul(sp) + 1, CSUBST) - sp) + beg;
                    /* ({) the } or x is already skipped */
                    if (end < wdscan(beg, EOS))
                        *end = EOS;
                    str = snptreef(NULL, 64, Tf_S, beg);
                    afree(beg, ATEMP);
                    kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, str,
                          Tbadsubst);
                }
                if (f & DOBLANK)
                    doblank++;
                tilde_ok = 0;
                if (word == IFS_QUOTE && type != XNULLSUB)
                    word = IFS_WORD;
                if (type == XBASE) {
                    /* expand? */
                    if (!st->next) {
                        SubType *newst;

                        newst = alloc(sizeof(SubType), ATEMP);
                        newst->next = NULL;
                        newst->prev = st;
                        st->next = newst;
                    }
                    st = st->next;
                    st->stype = stype;
                    st->base = Xsavepos(ds, dp);
                    st->f = f;
                    if (x.var == vtemp) {
                        st->var = tempvar(vtemp->name);
                        st->var->flag &= ~INTEGER;
                        /* can't fail here */
                        setstr(st->var, str_val(x.var), QSH_RETURN_ERROR | 0x4);
                    } else
                        st->var = x.var;

                    st->quotew = st->quotep = quote;
                    /* skip qualifier(s) */
                    if (stype)
                        sp += slen;
                    switch (stype & STYPE_SINGLE) {
                    case ORD('#') | STYPE_AT:
                    case ORD('Q') | STYPE_AT:
                    case ORD('^') | STYPE_AT:
                        break;
                    case ORD('0'): {
                        char *beg, *mid, *end, *stg;
                        qsh_ari_t from = 0, num = -1, flen, finc = 0;

                        beg = wdcopy(sp, ATEMP);
                        mid = beg + (wdscan(sp, ADELIM) - sp);
                        stg = beg + (wdscan(sp, CSUBST) - sp);
                        mid[-2] = EOS;
                        if (ord(mid[-1]) == ORD(/*{*/ '}')) {
                            sp += mid - beg - 1;
                            end = NULL;
                        } else {
                            end = mid + (wdscan(mid, ADELIM) - mid);
                            if (ord(end[-1]) != ORD(/*{*/ '}'))
                                /* more than max delimiters */
                                goto unwind_substsyn;
                            end[-2] = EOS;
                            sp += end - beg - 1;
                        }
                        evaluate(substitute(stg = wdstrip(beg, 0), 0), &from, QSH_UNWIND_ERROR, true);
                        afree(stg, ATEMP);
                        if (end) {
                            evaluate(substitute(stg = wdstrip(mid, 0), 0), &num, QSH_UNWIND_ERROR,
                                     true);
                            afree(stg, ATEMP);
                        }
                        afree(beg, ATEMP);
                        beg = str_val(st->var);
                        flen = utflen(beg);
                        if (from < 0) {
                            if (-from < flen)
                                finc = flen + from;
                        } else
                            finc = from < flen ? from : flen;
                        if (UTFMODE)
                            utfincptr(beg, &finc);
                        beg += finc;
                        flen = utflen(beg);
                        if (num < 0 || num > flen)
                            num = flen;
                        if (UTFMODE)
                            utfincptr(beg, &num);
                        strndupx(x.str, beg, num, ATEMP);
                        goto do_CSUBST;
                    }
                    case ORD('/') | STYPE_AT:
                    case ORD('/'): {
                        char *s, *p, *d, *sbeg;
                        char *pat = NULL, *rrep;
                        char fpat = 0, *tpat1, *tpat2;
                        char *ws, *wpat, *wrep, tch;
                        size_t rreplen;

                        s = ws = wdcopy(sp, ATEMP);
                        p = s + (wdscan(sp, ADELIM) - sp);
                        d = s + (wdscan(sp, CSUBST) - sp);
                        p[-2] = EOS;
                        if (ord(p[-1]) == ORD(/*{*/ '}'))
                            d = NULL;
                        else
                            d[-2] = EOS;
                        sp += (d ? d : p) - s - 1;
                        if (!(stype & STYPE_MASK) && s[0] == CHAR && ctype(s[1], C_SUB2))
                            fpat = s[1];
                        wpat = s + (fpat ? 2 : 0);
                        if (!(wrep = d ? p : NULL)) {
                            rrep = null;
                            rreplen = 0;
                        } else if (!(stype & STYPE_AT)) {
                            rrep = evalstr(wrep, DOTILDE | DOSCALAR);
                            rreplen = strlen(rrep);
                        } else {
                            rrep = NULL;
                            /* shut up GCC */
                            rreplen = 0;
                        }

                        /* prepare string on which to work */
                        strdupx(s, str_val(st->var), ATEMP);
                        sbeg = s;
                    again_search:
                        pat = evalstr(wpat, DOTILDE | DOSCALAR | DOPAT);
                        /* check for special cases */
                        if (!*pat && !fpat) {
                            /*
                             * empty unanchored
                             * pattern => reject
                             */
                            goto no_repl;
                        }
                        if ((stype & STYPE_MASK) && gmatchx(null, pat, false)) {
                            /*
                             * pattern matches empty
                             * string => don't loop
                             */
                            stype &= ~STYPE_MASK;
                        }

                        /* first see if we have any match at all */
                        if (ord(fpat) == ORD('#')) {
                            /* anchor at the beginning */
                            tpat1 = shf_smprintf("%s%c*", pat, MAGIC);
                            tpat2 = tpat1;
                        } else if (ord(fpat) == ORD('%')) {
                            /* anchor at the end */
                            tpat1 = shf_smprintf("%c*%s", MAGIC, pat);
                            tpat2 = pat;
                        } else {
                            /* float */
                            tpat1 = shf_smprintf("%c*%s%c*", MAGIC, pat, MAGIC);
                            tpat2 = tpat1 + 2;
                        }
                    again_repl:
                        /*
                         * this would not be necessary if gmatchx would return
                         * the start and end values of a match found, like re*
                         */
                        if (!gmatchx(sbeg, tpat1, false))
                            goto end_repl;
                        d = strnul(s);
                        /* now anchor the beginning of the match */
                        if (ord(fpat) != ORD('#'))
                            while (sbeg <= d) {
                                if (gmatchx(sbeg, tpat2, false))
                                    break;
                                else
                                    sbeg++;
                            }
                        /* now anchor the end of the match */
                        p = d;
                        if (ord(fpat) != ORD('%'))
                            while (p >= sbeg) {
                                bool gotmatch;

                                c = ord(*p);
                                *p = '\0';
                                gotmatch = ((bool)(gmatchx(sbeg, pat, false)));
                                *p = c;
                                if (gotmatch)
                                    break;
                                p--;
                            }

                        /* record partial string as match */
                        tch = *p;
                        *p = '\0';
                        record_match(sbeg);
                        *p = tch;
                        /* get replacement string, if necessary */
                        if ((stype & STYPE_AT) && rrep != null) {
                            afree(rrep, ATEMP);
                            /* might access match! */
                            rrep = evalstr(wrep, DOTILDE | DOSCALAR);
                            rreplen = strlen(rrep);
                        }

                        /*
                         * string:
                         * |--------|---------|-------\0
                         * s  n1    sbeg  n2  p  n3   d
                         *
                         * replacement:
                         *          |------------|
                         *          rrep  rreplen
                         */

                        /* move strings around and replace */
                        {
                            size_t n1 = sbeg - s;
                            size_t n2 = p - sbeg;
                            size_t n3 = d - p;
                            /* move part3 to the front, OR… */
                            if (rreplen < n2)
                                memmove(sbeg + rreplen, p, n3 + 1);
                            /* … adjust size, move to back */
                            if (rreplen > n2) {
                                s = aresize(s, n1 + rreplen + n3 + 1, ATEMP);
                                memmove(s + n1 + rreplen, s + n1 + n2, n3 + 1);
                            }
                            /* insert replacement */
                            if (rreplen)
                                memcpy(s + n1, rrep, rreplen);
                            /* continue after the place */
                            sbeg = s + n1 + rreplen;
                        }
                        if (stype & STYPE_AT) {
                            afree(tpat1, ATEMP);
                            afree(pat, ATEMP);
                            goto again_search;
                        } else if (stype & STYPE_DBL)
                            goto again_repl;
                    end_repl:
                        afree(tpat1, ATEMP);
                        x.str = s;
                    no_repl:
                        afree(pat, ATEMP);
                        if (rrep != null)
                            afree(rrep, ATEMP);
                        afree(ws, ATEMP);
                        goto do_CSUBST;
                    }
                    case ORD('#'):
                    case ORD('%'):
                        /* ! DOBLANK,DOBRACE */
                        f = (f & DONTRUNCOMMAND) | DOPAT | DOTILDE | DOTEMP | DOSCALAR;
                        tilde_ok = 1;
                        st->quotew = quote = 0;
                        /*
                         * Prepend open pattern (so |
                         * in a trim will work as
                         * expected)
                         */
                        if (!Flag(FSH)) {
                            *dp++ = MAGIC;
                            *dp++ = ORD(0x80 | '@');
                        }
                        break;
                    case ORD('='):
                        /*
                         * Tilde expansion for string
                         * variables in POSIX mode is
                         * governed by Austinbug 351.
                         * In non-POSIX mode historic
                         * ksh behaviour (enable it!)
                         * us followed.
                         * Not doing tilde expansion
                         * for integer variables is a
                         * non-POSIX thing - makes
                         * sense though, since ~ is
                         * a arithmetic operator.
                         */
                        if (!(x.var->flag & INTEGER))
                            f |= DOASNTILDE | DOTILDE;
                        f |= DOTEMP | DOSCALAR;
                        /*
                         * These will be done after the
                         * value has been assigned.
                         */
                        f &= ~(DOBLANK | DOGLOB | DOBRACE);
                        tilde_ok = 1;
                        break;
                    case ORD('?'):
                        if (*sp == CSUBST)
                            kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO,
                                  st->var->name, "parameter null or not set");
                        f &= ~DOBLANK;
                        f |= DOTEMP;
                        /* FALLTHROUGH */
                    default:
                        /* '-' '+' '?' */
                        if (quote)
                            word = IFS_WORD;
                        else if (dp == Xstring(ds, dp))
                            word = IFS_IWS;
                        /* Enable tilde expansion */
                        tilde_ok = 1;
                        f |= DOTILDE;
                    }
                } else
                    /* skip word */
                    sp += wdscan(sp, CSUBST) - sp;
                continue;
            }
            case CSUBST:
                /* only get here if expanding word */
            do_CSUBST:
                /* ({) skip the } or x */
                sp++;
                /* in case of ${unset:-} */
                tilde_ok = 0;
                *dp = '\0';
                quote = st->quotep;
                f = st->f;
                if (f & DOBLANK)
                    doblank--;
                switch (st->stype & STYPE_SINGLE) {
                case ORD('#'):
                case ORD('%'):
                    if (!Flag(FSH)) {
                        /* Append end-pattern */
                        *dp++ = MAGIC;
                        *dp++ = ')';
                    }
                    *dp = '\0';
                    dp = Xrestpos(ds, dp, st->base);
                    /*
                     * Must use st->var since calling
                     * global would break things
                     * like x[i+=1].
                     */
                    x.str = trimsub(str_val(st->var), dp, st->stype);
                    if (x.str[0] != '\0') {
                        word = IFS_IWS;
                        type = XSUB;
                    } else if (quote) {
                        word = IFS_WORD;
                        type = XSUB;
                    } else {
                        if (dp == Xstring(ds, dp))
                            word = IFS_IWS;
                        type = XNULLSUB;
                    }
                    if (f & DOBLANK)
                        doblank++;
                    st = st->prev;
                    continue;
                case ORD('='):
                    /*
                     * Restore our position and substitute
                     * the value of st->var (may not be
                     * the assigned value in the presence
                     * of integer/right-adj/etc attributes).
                     */
                    dp = Xrestpos(ds, dp, st->base);
                    /*
                     * Must use st->var since calling
                     * global would cause with things
                     * like x[i+=1] to be evaluated twice.
                     */
                    /*
                     * Note: not exported by FEXPORT
                     * in AT&T ksh.
                     */
                    /*
                     * XXX POSIX says read-only is only
                     * fatal for special builtins (setstr
                     * does read-only check).
                     */
                    len = strlen(dp) + 1;
                    setstr(st->var, debunk(alloc(len, ATEMP), dp, len), QSH_UNWIND_ERROR);
                    x.str = str_val(st->var);
                    type = XSUB;
                    if (f & DOBLANK)
                        doblank++;
                    st = st->prev;
                    word = quote || (!*x.str && (f & DOSCALAR)) ? IFS_WORD : IFS_IWS;
                    continue;
                case ORD('?'):
                    dp = Xrestpos(ds, dp, st->base);

                    kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO,
                          st->var->name, debunk(dp, dp, strlen(dp) + 1));
                    break;
                case ORD('#') | STYPE_AT:
                    x.str = shf_smprintf(Thex32, (unsigned int)hash(str_val(st->var)));
                    goto common_CSUBST;
                case ORD('Q') | STYPE_AT: {
                    struct shf shf;

                    shf_sopen(NULL, 0, SHF_WR | SHF_DYNAMIC, &shf);
                    print_value_quoted(&shf, str_val(st->var));
                    x.str = shf_sclose(&shf);
                    goto common_CSUBST;
                }
                case ORD('^') | STYPE_AT: {
                    struct shf shf;

                    shf_sopen(NULL, 0, SHF_WR | SHF_DYNAMIC, &shf);
                    uprntmbs(str_val(st->var), true, &shf);
                    x.str = shf_sclose(&shf);
                    goto common_CSUBST;
                }
                case ORD('0'):
                case ORD('/') | STYPE_AT:
                case ORD('/'):
                common_CSUBST:
                    dp = Xrestpos(ds, dp, st->base);
                    type = XSUB;
                    word = quote || (!*x.str && (f & DOSCALAR)) ? IFS_WORD : IFS_IWS;
                    if (f & DOBLANK)
                        doblank++;
                    st = st->prev;
                    continue;
                    /* default: '-' '+' */
                }
                st = st->prev;
                type = XBASE;
                continue;

            case OPAT:
                /* open pattern: *(foo|bar) */
                /* Next char is the type of pattern */
                make_magic = true;
                c = ord(*sp++) | 0x80U;
                break;

            case SPAT:
                /* pattern separator (|) */
                make_magic = true;
                c = ORD('|');
                break;

            case CPAT:
                /* close pattern */
                make_magic = true;
                c = ORD(/*(*/ ')');
                break;
            }
            break;

        case XNULLSUB:
            /*
             * Special case for "$@" (and "${foo[@]}") - no
             * word is generated if $# is 0 (unless there is
             * other stuff inside the quotes).
             */
            type = XBASE;
            if (f & DOBLANK) {
                doblank--;
                if (dp == Xstring(ds, dp) && word != IFS_WORD)
                    word = IFS_IWS;
            }
            continue;

        case XSUBPAT:
        case XSUBPATMID:
        XSUBPAT_beg:
            switch ((c = ord(*x.str++))) {
            case 0:
                goto XSUB_end;
            case ORD('\\'):
                if ((c = ord(*x.str)) == 0)
                    /* keep backslash at EOS */
                    c = ORD('\\');
                else
                    ++x.str;
                quote |= 2;
                break;
            /* ctype(c, C_PATMO) */
            case ORD('!'):
            case ORD('*'):
            case ORD('+'):
            case ORD('?'):
            case ORD('@'):
                if (ord(*x.str) == ORD('(' /*)*/)) {
                    ++x.str;
                    c |= 0x80U;
                    make_magic = true;
                }
                break;
            case ORD('('):
                c = ORD(' ') | 0x80U;
                /* FALLTHROUGH */
            case ORD('|'):
            case ORD(')'):
                make_magic = true;
                break;
            }
            break;

        case XSUB:
            if (!quote && (f & DODBMAGIC)) {
                const char *cs = x.str;
                int level = 0;

                while ((c = *cs++))
                    switch (c) {
                    case '\\':
                        if ((c = *cs))
                            ++cs;
                        break;
                    case ORD('('):
                        ++level;
                        break;
                    case ORD(')'):
                        --level;
                        break;
                    }
                /* balanced parentheses? */
                if (!level) {
                    type = XSUBPAT;
                    goto XSUBPAT_beg;
                }
            }
            /* FALLTHROUGH */
        case XSUBMID:
            if ((c = ord(*x.str++)) == 0) {
            XSUB_end:
                type = XBASE;
                if (f & DOBLANK)
                    doblank--;
                continue;
            }
            break;

        case XARGSEP:
            type = XARG;
            quote = 1;
            /* FALLTHROUGH */
        case XARG:
            if ((c = ord(*x.str++)) == '\0') {
                /*
                 * force null words to be created so
                 * set -- "" 2 ""; echo "$@" will do
                 * the right thing
                 */
                if (quote && x.split)
                    word = IFS_WORD;
                if ((x.str = *x.u.strv++) == NULL) {
                    type = XBASE;
                    if (f & DOBLANK)
                        doblank--;
                    continue;
                }
                c = ord(ifs0);
                if ((f & DOHEREDOC)) {
                    /* pseudo-field-split reliably */
                    if (c == 0)
                        c = ORD(' ');
                    break;
                }
                if ((f & DOSCALAR)) {
                    /* do not field-split */
                    if (x.split) {
                        c = ORD(' ');
                        break;
                    }
                    if (c == 0)
                        continue;
                }
                if (c == 0) {
                    if (quote && !x.split)
                        continue;
                    if (!quote && word == IFS_WS)
                        continue;
                    /* this is so we don't terminate */
                    c = ORD(' ');
                    /* now force-emit a word */
                    goto emit_word;
                }
                if (quote && x.split) {
                    /* terminate word for "$@" */
                    type = XARGSEP;
                    quote = 0;
                }
            }
            break;

        case XCOM:
            if (x.u.shf == NULL) {
                /* $(<...) failed */
                subst_exstat = 1;
                /* fake EOF */
                c = -1;
            } else if (newlines) {
                /* spit out saved NLs */
                c = ORD('\n');
                --newlines;
            } else {
                while (c = shf_getc(x.u.shf), cinttype(c, C_NL | C_NUL)) {
                    if (c == ORD('\n'))
                        /* save newlines */
                        newlines++;
                }
                if (newlines && c != -1) {
                    shf_ungetc(c, x.u.shf);
                    c = ORD('\n');
                    --newlines;
                }
            }
            if (c == -1) {
                newlines = 0;
                if (x.u.shf)
                    shf_close(x.u.shf);
                if (x.split)
                    subst_exstat = waitlast();
                type = XBASE;
                if (f & DOBLANK)
                    doblank--;
                continue;
            }
            break;
        }

        /* check for end of word or IFS separation */
        if (c == 0 || (!quote && (f & DOBLANK) && doblank && !make_magic && ctype(c, C_IFS))) {
            /*-
             * How words are broken up:
             *          |   value of c
             *  word        |   ws  nws 0
             *  -----------------------------------
             *  IFS_WORD        w/WS    w/NWS   w
             *  IFS_WS          -/WS    -/NWS   -
             *  IFS_NWS         -/NWS   w/NWS   -
             *  IFS_IWS         -/WS    w/NWS   -
             * (w means generate a word)
             */
            if ((word == IFS_WORD) || (word == IFS_QUOTE) ||
                (c && (word == IFS_IWS || word == IFS_NWS) && !ctype(c, C_IFSWS))) {
                size_t dlen;
            emit_word:
                if (f & DOHERESTR)
                    *dp++ = '\n';
                *dp++ = '\0';
                dlen = Xlength(ds, dp);
                cp = Xclose(ds, dp);
                if (fdo & DOBRACE)
                    /* also does globbing */
                    alt_expand(wp, cp, cp, cp + dlen - 1, fdo | (f & DOMARKDIRS));
                else if (fdo & DOGLOB)
                    glob(cp, wp, ((bool)(f & DOMARKDIRS)));
                else if ((f & DOPAT) || !(fdo & DOMAGIC))
                    XPput(*wp, cp);
                else
                    XPput(*wp, debunk(cp, cp, dlen));
                fdo = 0;
                saw_eq = false;
                /* must be 1/0 */
                tilde_ok = (f & (DOTILDE | DOASNTILDE)) ? 1 : 0;
                if (c == 0)
                    return;
                Xinit(ds, dp, 128, ATEMP);
            } else if (c == 0) {
                return;
            } else if (isXSUB(type) && ctype(c, C_IFS) && !ctype(c, C_IFSWS) &&
                       Xlength(ds, dp) == 0) {
                *(cp = alloc(1, ATEMP)) = '\0';
                XPput(*wp, cp);
                ++type;
            }
            if (word != IFS_NWS)
                word = ctype(c, C_IFSWS) ? IFS_WS : IFS_NWS;
        } else {
            if (isXSUB(type))
                ++type;

            /* age tilde_ok info - ~ code tests second bit */
            tilde_ok <<= 1;
            /* mark any special second pass chars */
            if (!quote)
                switch (ord(c)) {
                case ORD('['):
                case ORD('!'):
                case ORD('-'):
                case ORD(']'):
                    /*
                     * For character classes - doesn't hurt
                     * to have magic !,-,]s outside of
                     * [...] expressions.
                     */
                    if (f & (DOPAT | DOGLOB)) {
                        fdo |= DOMAGIC;
                        if ((unsigned int)c == ORD('['))
                            fdo |= f & DOGLOB;
                        *dp++ = MAGIC;
                    }
                    break;
                case ORD('*'):
                case ORD('?'):
                    if (f & (DOPAT | DOGLOB)) {
                        fdo |= DOMAGIC | (f & DOGLOB);
                        *dp++ = MAGIC;
                    }
                    break;
                case ORD('{'):
                case ORD('}'):
                case ORD(','):
                    if ((f & DOBRACE) && (ord(c) == ORD('{' /*}*/) || (fdo & DOBRACE))) {
                        fdo |= DOBRACE | DOMAGIC;
                        *dp++ = MAGIC;
                    }
                    break;
                case ORD('='):
                    /* Note first unquoted = for ~ */
                    if (!(f & DOTEMP) && (!Flag(FPOSIX) || (f & DOASNTILDE)) && !saw_eq) {
                        saw_eq = true;
                        tilde_ok = 1;
                    }
                    break;
                case ORD(':'):
                    /* : */
                    /* Note unquoted : for ~ */
                    if (!(f & DOTEMP) && (f & DOASNTILDE))
                        tilde_ok = 1;
                    break;
                case ORD('~'):
                    /*
                     * tilde_ok is reset whenever
                     * any of ' " $( $(( ${ } are seen.
                     * Note that tilde_ok must be preserved
                     * through the sequence ${A=a=}~
                     */
                    if (type == XBASE && (f & (DOTILDE | DOASNTILDE)) && (tilde_ok & 2)) {
                        const char *tcp;
                        char *tdp = dp;

                        tcp = maybe_expand_tilde(sp, &ds, &tdp, ((bool)(f & DOASNTILDE)));
                        if (tcp) {
                            if (dp != tdp)
                                word = IFS_WORD;
                            dp = tdp;
                            sp = tcp;
                            continue;
                        }
                    }
                    break;
                }
            else
                /* undo temporary */
                quote &= ~2;

            if (make_magic) {
                make_magic = false;
                fdo |= DOMAGIC | (f & DOGLOB);
                *dp++ = MAGIC;
            } else if (ISMAGIC(c)) {
                fdo |= DOMAGIC;
                *dp++ = MAGIC;
            }
            /* save output char */
            *dp++ = c;
            word = IFS_WORD;
        }
    }
}

