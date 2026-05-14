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

/*-
 * Variables
 *
 * WARNING: unreadable code, needs a rewrite
 *
 * if (flag&INTEGER), val.i contains integer value, and type contains base.
 * otherwise, (val.s + type) contains string value.
 * if (flag&EXPORT), val.s contains "name=value" for E-Z exporting.
 */

#include "var_priv.h"  /* enum var_specs, qsh_ari_u, shared decls */

struct table specials;            /* shared with var_special.c */
k32 lcg_state = 5381U;             /* RNG state, shared with var_special.c */
k32 qh_state = 4711U;              /* shared with var_special.c */
/* may only be set by typeset() just before call to array_index_calc() */
static enum namerefflag innermost_refflag = SRF_NOP;

/* c_typeset_vardump now in var_priv.h */
/* c_typeset_vardump_recursive now in var_priv.h */
static char *formatstr(struct tbl *, const char *);
static void exportprep(struct tbl *, const char *, size_t);
extern int special(const char *);
static void unspecial(const char *);
extern void getspec(struct tbl *);
extern void setspec(struct tbl *);
extern void unsetspec(struct tbl *, bool);
/* getint/getnum forward decls live in var_priv.h (called from var_special.c) */
static const char *array_index_calc(const char *, bool *, k32 *);
/* vtypeset now in var_priv.h */

/*
 * create a new block for function calls and simple commands
 * assume caller has allocated and set up e->loc
 */
/* pre-initio() */
void
newblock(void)
{
    struct block *l;
    static const char *empty[] = {null, NULL};

    l = alloc(sizeof(struct block), ATEMP);
    l->flags = 0;
    /* TODO: could use e->area (l->area => l->areap) */
    ainit(&l->area);
    if (!e->loc) {
        l->argc = 0;
        l->argv = empty;
    } else {
        l->argc = e->loc->argc;
        l->argv = e->loc->argv;
    }
    l->exit = l->error = NULL;
    ktinit(&l->area, &l->vars, 0);
    ktinit(&l->area, &l->funs, 0);
    l->next = e->loc;
    e->loc = l;
}

/*
 * pop a block handling special variables
 */
void
popblock(void)
{
    ssize_t i;
    struct block *l = e->loc;
    struct tbl *vp, **vpp = l->vars.tbls, *vq;

    /* pop block */
    e->loc = l->next;

    i = 1 << (l->vars.tshift);
    while (--i >= 0)
        if ((vp = *vpp++) != NULL && (vp->flag & SPECIAL)) {
            if ((vq = global(vp->name))->flag & ISSET)
                setspec(vq);
            else
                unsetspec(vq, false);
        }
    if (l->flags & BF_DOGETOPTS)
        user_opt = l->getopts_state;
    afreeall(&l->area);
    afree(l, ATEMP);
}

/* called by main() to initialise variable data structures.
 * Biased -1 relative to VARSPEC_ENUMS. */
static const char *const initvar_names[] = {
#define VARSPEC_ITEMS
#include "var_spec.h"
};

void
initvar(void)
{
    int i = 0;
    struct tbl *tp;

    ktinit(APERM, &specials,
           /* currently 21 specials: 75% of 32 = 2^5 */
           5);
    while (i < V_MAX - 1) {
        tp = ktenter(&specials, initvar_names[i], hash(initvar_names[i]));
        tp->flag = DEFINED | ISSET;
        tp->type = ++i;
    }
}

/* common code for several functions below and c_typeset() */
struct block *
varsearch(struct block *l, struct tbl **vpp, const char *vn, k32 h)
{
    register struct tbl *vp;

    if (l) {
    varsearch_loop:
        if ((vp = ktsearch(&l->vars, vn, h)) != NULL)
            goto varsearch_out;
        if (l->next != NULL) {
            l = l->next;
            goto varsearch_loop;
        }
    }
    vp = NULL;
varsearch_out:
    *vpp = vp;
    return (l);
}

/*
 * Used to calculate an array index for global()/local(). Sets *arrayp
 * to true if this is an array, sets *idxp to the array index, returns
 * the basename of the array. May only be called from global()/local()
 * and must be their first callee.
 */
static const char *
array_index_calc(const char *n, bool *arrayp, k32 *idxp)
{
    const char *p;
    size_t len;
    char *ap = NULL;

    *arrayp = false;
redo_from_ref:
    p = skip_varname(n, false);
    if ((size_t)(p - n) > (size_t)(INT_MAX - X_EXTRA))
        kerrf(KWF_ERR(255) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG | KWF_NOERRNO,
              "parameter name too long");
    if (innermost_refflag == SRF_NOP && (p != n) && ctype(n[0], C_ALPHX)) {
        struct tbl *vp;
        char *vn;

        strndupx(vn, n, p - n, ATEMP);
        /* check if this is a reference */
        varsearch(e->loc, &vp, vn, hash(vn));
        afree(vn, ATEMP);
        if (vp && (vp->flag & (DEFINED | ASSOC | ARRAY)) == (DEFINED | ASSOC)) {
            char *cp;

            /* gotcha! */
            strdup2x(cp, str_val(vp), p);
            afree(ap, ATEMP);
            n = ap = cp;
            goto redo_from_ref;
        }
    }
    innermost_refflag = SRF_NOP;

    if (p != n && ord(*p) == ORD('[') && (len = array_ref_len(p))) {
        char *sub, *tmp;
        qsh_ari_u rval;
        size_t tmplen = p - n;

        /* calculate the value of the subscript */
        *arrayp = true;
        len -= 2;
        tmp = alloc((len > tmplen ? len : tmplen) + 1, ATEMP);
        memcpy(tmp, p + 1, len);
        tmp[len] = '\0';
        sub = substitute(tmp, 0);
        evaluate(sub, &rval.i, QSH_UNWIND_ERROR, true);
        *idxp = qiMM(k32, K32_FM, rval.u);
        afree(sub, ATEMP);
        memcpy(tmp, n, tmplen);
        tmp[tmplen] = '\0';
        n = tmp;
    }
    return (n);
}

#define vn vname.ro
/*
 * Search for variable, if not found create globally.
 */
struct tbl *
global(const char *n)
{
    return (isglobal(n, true));
}

/* search for variable; if not found, return NULL or create globally */
struct tbl *
isglobal(const char *n, bool docreate)
{
    struct tbl *vp;
    union qsh_cchack vname;
    struct block *l = e->loc;
    int c;
    bool array;
    k32 h;
    k32 idx;

    /*
     * check to see if this is an array;
     * dereference namerefs; must come first
     */
    vn = array_index_calc(n, &array, &idx);
    h = hash(vn);
    c = (unsigned char)vn[0];
    if (!ctype(c, C_ALPHX)) {
        if (array)
            kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG | KWF_NOERRNO, Tbadsubst);
        vp = vtemp;
        vp->flag = DEFINED;
        vp->type = 0;
        vp->areap = ATEMP;
        if (ctype(c, C_DIGIT)) {
            if (getn(vn, &c)) {
                /* main.c:main_init() says 12 */
                shf_snprintf(vp->name, 12, Tf_d, c);
                if (c <= l->argc) {
                    /* setstr can't fail here */
                    setstr(vp, l->argv[c], QSH_RETURN_ERROR);
                }
            } else
                vp->name[0] = '\0';
            vp->flag |= RDONLY;
            goto out;
        }
        vp->name[0] = c;
        vp->name[1] = '\0';
        vp->flag |= RDONLY;
        if (!c || vn[1] != '\0')
            goto out;
        vp->flag |= ISSET | INTEGER;
        switch (c) {
        case '$':
            vp->val.i = qshpid;
            break;
        case '!':
            /* if no job, expand to nothing */
            if ((vp->val.i = j_async()) == 0)
                vp->flag &= ~(ISSET | INTEGER);
            break;
        case '?':
            vp->val.i = exstat & 0xFF;
            break;
        case '#':
            vp->val.i = l->argc;
            break;
        case '-':
            vp->flag &= ~INTEGER;
            vp->val.s = getoptions();
            break;
        default:
            vp->flag &= ~(ISSET | INTEGER);
        }
        goto out;
    }
    l = varsearch(e->loc, &vp, vn, h);
    if (vp == NULL && docreate)
        vp = ktenter(&l->vars, vn, h);
    else
        docreate = false;
    if (vp != NULL) {
        if (array)
            vp = arraysearch(vp, idx);
        if (docreate) {
            vp->flag |= DEFINED;
            if (special(vn))
                vp->flag |= SPECIAL;
        }
    }
out:
    last_lookup_was_array = array;
    if (vn != n)
        afree(vname.rw, ATEMP);
    return (vp);
}

/*
 * Search for local variable, if not found create locally.
 */
struct tbl *
local(const char *n, bool copy)
{
    struct tbl *vp;
    union qsh_cchack vname;
    struct block *l = e->loc;
    bool array;
    k32 h;
    k32 idx;

    /*
     * check to see if this is an array;
     * dereference namerefs; must come first
     */
    vn = array_index_calc(n, &array, &idx);
    h = hash(vn);
    if (!ctype(*vn, C_ALPHX)) {
        vp = vtemp;
        vp->flag = DEFINED | RDONLY;
        vp->type = 0;
        vp->areap = ATEMP;
        goto out;
    }
    vp = ktenter(&l->vars, vn, h);
    if (copy && !(vp->flag & DEFINED)) {
        struct tbl *vq;

        varsearch(l->next, &vq, vn, h);
        if (vq != NULL) {
            vp->flag |= vq->flag & (EXPORT | INTEGER | RDONLY | LJUST | RJUST | ZEROFIL | LCASEV |
                                    UCASEV_AL | INT_U | INT_L);
            if (vq->flag & INTEGER)
                vp->type = vq->type;
            vp->u2.field = vq->u2.field;
        }
    }
    if (array)
        vp = arraysearch(vp, idx);
    vp->flag |= DEFINED;
    if (special(vn))
        vp->flag |= SPECIAL;
out:
    last_lookup_was_array = array;
    if (vn != n)
        afree(vname.rw, ATEMP);
    return (vp);
}
#undef vn

/* get variable string value */
char *
str_val(struct tbl *vp)
{
    char *s;

    if ((vp->flag & SPECIAL))
        getspec(vp);
    if (!(vp->flag & ISSET))
        /* special to dollar() */
        s = null;
    else if (!(vp->flag & INTEGER))
        /* string source */
        s = vp->val.s + vp->type;
    else {
        /* integer source */
        qsh_uari_t n;
        unsigned int base;
        /**
         * worst case number length is when base == 2:
         *  1 (minus) + 2 (base, up to 36) + 1 ('#') +
         *  number of bits in the qsh_uari_t + 1 (NUL)
         */
        char strbuf[1 + 2 + 1 + 8 * sizeof(qsh_uari_t) + 1];
        const char *digits = (vp->flag & UCASEV_AL) ? digits_uc : digits_lc;

        s = strbuf + sizeof(strbuf);
        if (vp->flag & INT_U)
            n = vp->val.u;
        else
            n = (vp->val.i < 0) ? -vp->val.u : vp->val.u;
        base = (vp->type == 0) ? 10U : (unsigned int)vp->type;

        if (base == 1) {
            s = strbuf;
            s[1] = '#';
            if (n == 0) {
                s[0] = '2';
                s[2] = '0';
                s[3] = '\0';
            } else {
                s[0] = '1';
                s[2 + ez_ctomb(s + 2, n)] = '\0';
            }
        } else {
            *--s = '\0';
            do {
                *--s = digits[n % base];
                n /= base;
            } while (n != 0);
            if (base != 10) {
                *--s = '#';
                *--s = digits[base % 10];
                if (base >= 10)
                    *--s = digits[base / 10];
            }
            if (!(vp->flag & INT_U) && vp->val.i < 0)
                *--s = '-';
        }
        if (vp->flag & (RJUST | LJUST))
            /* case already dealt with */
            s = formatstr(vp, s);
        else
            strdupx(s, s, ATEMP);
    }
    return (s);
}

/* set variable to string value */
int
setstr(struct tbl *vq, const char *s, int error_ok)
{
    bool no_ro_check = ((bool)(error_ok & 0x4));

    error_ok &= ~0x4;
    if ((vq->flag & RDONLY) && !no_ro_check) {
        kwarnf((error_ok ? KWF_WARNING : KWF_ERR(2)) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG |
                   KWF_NOERRNO,
               Tread_only, vq->name);
        if (!error_ok)
            unwind(LERROR);
        return (0);
    }
    if (!(vq->flag & INTEGER)) {
        /* string dest */
        char *salloc = NULL;
        size_t cursz;
        qiPTR_U cmp_s, cmp_b, cmp_e;

        if ((vq->flag & ALLOC)) {
            cursz = strlen(vq->val.s) + 1;
            /* debugging */
            cmp_s = (qiPTR_U)(const void *)s;
            cmp_b = (qiPTR_U)(void *)vq->val.s;
            cmp_e = (qiPTR_U)(void *)(vq->val.s + cursz);
            if (cmp_s >= cmp_b && cmp_s < cmp_e) {
                kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO,
                       "setstr: %s=%s: assigning to self", vq->name, s);
            }
        } else
            cursz = 0;
        if (s && (vq->flag & (UCASEV_AL | LCASEV | LJUST | RJUST)))
            s = salloc = formatstr(vq, s);
        if ((vq->flag & EXPORT))
            exportprep(vq, s, cursz);
        else {
            size_t n = strlen(s) + 1;
            vq->val.s = aresizeif(cursz, (vq->flag & ALLOC) ? vq->val.s : NULL, n, vq->areap);
            memcpy(vq->val.s, s, n);
            vq->flag |= ALLOC;
            vq->type = 0;
        }
        vq->flag &= ~IMPORT;
        afree(salloc, ATEMP);
    } else {
        /* integer dest */
        if (!v_evaluate(vq, s, error_ok, true))
            return (0);
    }
    vq->flag |= ISSET;
    if ((vq->flag & SPECIAL))
        setspec(vq);
    return (1);
}

/* set variable to integer */
void
setint(struct tbl *vq, qsh_ari_t n)
{
    if (!(vq->flag & INTEGER)) {
        vtemp->flag = (ISSET | INTEGER);
        vtemp->type = 0;
        vtemp->areap = ATEMP;
        vtemp->val.i = n;
        /* setstr can't fail here */
        setstr(vq, str_val(vtemp), QSH_RETURN_ERROR);
    } else
        vq->val.i = n;
    vq->flag |= ISSET;
    if ((vq->flag & SPECIAL))
        setspec(vq);
}

int
getint(struct tbl *vp, qsh_ari_u *nump, bool arith)
{
    if (vp->flag & SPECIAL)
        getspec(vp);
    /* XXX is it possible for ISSET to be set and val.s to be NULL? */
    if (!(vp->flag & ISSET) || (!(vp->flag & INTEGER) && vp->val.s == NULL)) {
        errno = EINVAL;
        return (-1);
    }
    if (vp->flag & INTEGER) {
        nump->i = vp->val.i;
        return (vp->type);
    }
    return (getnum(vp->val.s + vp->type, nump, arith, Flag(FPOSIX) && !(vp->flag & ZEROFIL)));
}

int
getnum(const char *s, qsh_ari_u *nump, bool arith, bool psxoctal)
{
    qsh_uari_t c, num = 0, base = 10;
    bool have_base = false, neg = false;

    do {
        c = (unsigned char)*s++;
    } while (ctype(c, C_SPACE));

    switch (c) {
    case '-':
        neg = true;
        /* FALLTHROUGH */
    case '+':
        c = (unsigned char)*s++;
        break;
    }

    if (c == '0' && arith) {
        if (isCh(s[0], 'X', 'x')) {
            /* interpret as hexadecimal */
            base = 16;
            ++s;
            goto getint_c_style_base;
        } else if (psxoctal && ctype(s[0], C_DIGIT)) {
            /* interpret as octal (deprecated) */
            base = 8;
        getint_c_style_base:
            have_base = true;
            c = (unsigned char)*s++;
        }
    }

    do {
        if (c == '#') {
            /* ksh-style base determination */
            if (have_base || num < 1) {
                errno = EINVAL;
                return (-1);
            }
            if ((base = num) == 1) {
                /* mksh-specific extension */
                unsigned int wc;

                ez_mbtoc(&wc, s);
                nump->u = (qsh_uari_t)wc;
                return (1);
            } else if (base > 36)
                base = 10;
            num = 0;
            have_base = true;
            continue;
        }
        if (ctype(c, C_DIGIT))
            c = qsh_numdig(c);
        else if (ctype(c, C_UPPER))
            c = qsh_numuc(c) + 10;
        else if (ctype(c, C_LOWER))
            c = qsh_numlc(c) + 10;
        else {
            errno = EINVAL;
            return (-1);
        }
        if (c >= base) {
            errno = EINVAL;
            return (-1);
        }
        /* handle overflow as truncation */
        num = num * base + c;
    } while ((c = (unsigned char)*s++));

    if (neg)
        num = -num;
    nump->u = num;
    return (base);
}

/*
 * convert variable vq to integer variable, setting its value from vp
 * (vq and vp may be the same)
 */
struct tbl *
setint_v(struct tbl *vq, struct tbl *vp, bool arith)
{
    int base;
    qsh_ari_u num;

    if ((base = getint(vp, &num, arith)) == -1)
        return (NULL);
    setint_n(vq, num.i, 0);
    if (vq->type == 0)
        /* default base */
        vq->type = base;
    return (vq);
}

/* convert variable vq to integer variable, setting its value to num */
void
setint_n(struct tbl *vq, qsh_ari_t num, int newbase)
{
    if (!(vq->flag & INTEGER)) {
        if (vq->flag & ALLOC)
            afree(vq->val.s, vq->areap);
        vq->flag &= ~(ALLOC | IMPORT);
        vq->type = 0;
    }
    vq->val.i = num;
    if (newbase != 0)
        vq->type = newbase;
    vq->flag |= ISSET | INTEGER;
    if (vq->flag & SPECIAL)
        setspec(vq);
}

static char *
formatstr(struct tbl *vp, const char *s)
{
    char *p, *q;

    if (vp->flag & (RJUST | LJUST)) {
        int slen, nlen;
        size_t psiz;

        psiz = utf_mbswidth(s);
        if (psiz > (size_t)INT_MAX) {
            errno = EOVERFLOW;
            kerrf0(KWF_ERR(0xFF) | KWF_PREFIX | KWF_FILELINE, "string width %zu", psiz);
        }
        slen = (int)psiz;
        if (!vp->u2.field)
            /* default field width */
            vp->u2.field = slen;
        nlen = vp->u2.field;

        p = alloc2(nlen + 1, /* MB_LEN_MAX */ 4, ATEMP);
        psiz = ((size_t)nlen + 1U) * 4U;

        if (vp->flag & RJUST) {
            const char *qq;
            int n = 0;

            qq = s + strlen(s);

            /* strip trailing spaces (AT&T uses qq[-1] == ' ') */
            while (qq > s && ctype(qq[-1], C_SPACE)) {
                --qq;
                --slen;
            }
            if (HAS(vp->flag, ZEROFIL | INTEGER)) {
                if (!s[0] || !s[1])
                    goto uhm_no;
                if (s[1] == '#')
                    n = 2;
                else if (s[2] == '#')
                    n = 3;
            uhm_no:
                if (vp->u2.field <= n)
                    n = 0;
            }
            if (n) {
                memcpy(p, s, n);
                s += n;
            }
            while (slen > vp->u2.field)
                slen -= utf_widthadj(s, &s);
            if (vp->u2.field - slen)
                memset(p + n, (vp->flag & ZEROFIL) ? '0' : ' ', vp->u2.field - slen);
            slen -= n;
            shf_snprintf(p + vp->u2.field - slen, psiz - (vp->u2.field - slen), "%.*s", slen, s);
        } else {
            /* strip leading spaces/zeros */
            while (ctype(*s, C_SPACE))
                s++;
            if (vp->flag & ZEROFIL)
                while (*s == '0')
                    s++;
            shf_snprintf(p, psiz, "%-*.*s", vp->u2.field, vp->u2.field, s);
        }
    } else
        strdupx(p, s, ATEMP);

    if (vp->flag & UCASEV_AL) {
        for (q = p; *q; q++)
            *q = qsh_toupper(*q);
    } else if (vp->flag & LCASEV) {
        for (q = p; *q; q++)
            *q = qsh_tolower(*q);
    }

    return (p);
}

/*
 * make vp->val.s be "name=value" for quick exporting.
 */
static void
exportprep(struct tbl *vp, const char *val, size_t cursz)
{
    char *cp = (vp->flag & ALLOC) ? vp->val.s : NULL;
    size_t namelen = strlen(vp->name);
    size_t vallen = strlen(val) + 1;

    vp->flag |= ALLOC;
    vp->type = namelen + 1;
    /* since name+val are both in memory this can go unchecked */
    vp->val.s = aresizeif(cursz, cp, vp->type + vallen, vp->areap);
    memmove(vp->val.s + vp->type, val == cp ? vp->val.s : val, vallen);
    memcpy(vp->val.s, vp->name, namelen);
    ((char *)(vp->val.s))[namelen] = '=';
}

/*
 * lookup variable (according to (set&LOCAL)), set its attributes
 * (INTEGER, RDONLY, EXPORT, TRACE, LJUST, RJUST, ZEROFIL, LCASEV,
 * UCASEV_AL), and optionally set its value if an assignment.
 */
struct tbl *
typeset(const char *var, kui set, kui clr, int field, int base)
{
    return (vtypeset(NULL, var, set, clr, field, base));
}
struct tbl *
vtypeset(int *ep, const char *var, kui set, kui clr, int field, int base)
{
    struct tbl *vp;
    struct tbl *vpbase, *t;
    char *tvar, tvarbuf[32];
    const char *val;
    size_t len;
    bool vappend = false;
    enum namerefflag new_refflag = SRF_NOP;

    if (ep)
        *ep = 0;

    if ((set & (ARRAY | ASSOC)) == ASSOC) {
        new_refflag = SRF_ENABLE;
        set &= ~(ARRAY | ASSOC);
    }
    if ((clr & (ARRAY | ASSOC)) == ASSOC) {
        new_refflag = SRF_DISABLE;
        clr &= ~(ARRAY | ASSOC);
    }

    /* check for valid variable name, search for value */
    val = skip_varname(var, false);
    if (val == var) {
        /* no variable name given */
        return (NULL);
    }
    if (ord(*val) == ORD('[')) {
        if (new_refflag != SRF_NOP)
            merrf(NULL, (ep, KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, var,
                         "reference variable can't be an array"));
        len = array_ref_len(val);
        if (len < 3)
            return (NULL);
        /*
         * IMPORT is only used when the shell starts up and is
         * setting up its environment. Allow only simple array
         * references at this time since parameter/command
         * substitution is performed on the [expression] which
         * would be a major security hole.
         */
        if (set & IMPORT) {
            qsh_ari_u num;

            len -= 2;
            strnbdupx(tvar, val + 1, len, ATEMP, tvarbuf);
            if (getnum(tvar, &num, true, false) == -1)
                len = 0;
            if (tvar != tvarbuf)
                afree(tvar, ATEMP);
            if (!len)
                return (NULL);
            len += 2;
        }
        val += len;
    }
    if (ord(val[0]) == ORD('=')) {
        len = val - var;
        strnbdupx(tvar, var, len, ATEMP, tvarbuf);
        ++val;
    } else if (set & IMPORT) {
        /* environment invalid variable name or no assignment */
        return (NULL);
    } else if (ord(val[0]) == ORD('+') && ord(val[1]) == ORD('=')) {
        len = val - var;
        strnbdupx(tvar, var, len, ATEMP, tvarbuf);
        val += 2;
        vappend = true;
    } else if (val[0] != '\0') {
        /* other invalid variable names (not from environment) */
        return (NULL);
    } else {
        /* just varname with no value part nor equals sign */
        len = strlen(var);
        strnbdupx(tvar, var, len, ATEMP, tvarbuf);
        val = NULL;
        /* handle foo[*] => foo (whole array) mapping for R39b */
        if (len > 3 && ord(tvar[len - 3]) == ORD('[') && ord(tvar[len - 2]) == ORD('*') &&
            ord(tvar[len - 1]) == ORD(']'))
            tvar[len - 3] = '\0';
    }

    if (new_refflag == SRF_ENABLE) {
        const char *qval, *ccp;

        /* bail out on 'nameref foo+=bar' */
        if (vappend)
            merrf(NULL, (ep, KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG | KWF_NOERRNO,
                         "appending not allowed for nameref"));
        /* find value if variable already exists */
        if ((qval = val) == NULL) {
            varsearch(e->loc, &vp, tvar, hash(tvar));
            if (vp == NULL)
                goto nameref_empty;
            qval = str_val(vp);
        }
        /* check target value for being a valid variable name */
        ccp = skip_varname(qval, false);
        if (ccp == qval) {
            int c;

            if (!(c = (unsigned char)qval[0]))
                goto nameref_empty;
            else if (ctype(c, C_DIGIT) && getn(qval, &c))
                goto nameref_rhs_checked;
            else if (qval[1] == '\0')
                switch (c) {
                case '$':
                case '!':
                case '?':
                case '#':
                case '-':
                    goto nameref_rhs_checked;
                }
        nameref_empty:
            merrf(NULL, (ep, KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, var,
                         "empty nameref target"));
        }
        len = (ord(*ccp) == ORD('[')) ? array_ref_len(ccp) : 0;
        if (ccp[len]) {
            /*
             * works for cases "no array", "valid array with
             * junk after it" and "invalid array"; in the
             * latter case, len is also 0 and points to '['
             */
            merrf(NULL, (ep, KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO,
                         qval, "nameref target not a valid parameter name"));
        }
    nameref_rhs_checked:
        /* prevent nameref loops */
        while (qval) {
            if (!strcmp(qval, tvar))
                merrf(NULL, (ep, KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO,
                             qval, "expression recurses on parameter"));
            varsearch(e->loc, &vp, qval, hash(qval));
            qval = NULL;
            if (vp && ((vp->flag & (ARRAY | ASSOC)) == ASSOC))
                qval = str_val(vp);
        }
    }

    /* prevent typeset from creating a local PATH/ENV/SHELL */
    if (Flag(FRESTRICTED) &&
        (strcmp(tvar, TPATH) == 0 || strcmp(tvar, TENV) == 0 || strcmp(tvar, TSHELL) == 0))
        merrf(NULL, (ep, KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, tvar,
                     "restricted"));

    innermost_refflag = new_refflag;
    vp = (set & LOCAL) ? local(tvar, ((bool)(set & LOCAL_COPY))) : global(tvar);
    /* when importing environment, resolve duplicates as first-wins */
    /* the EXPORT check is to permit overwriting the default $PATH */
    if ((set & IMPORT) && (vp->flag & (ISSET | EXPORT)) == (ISSET | EXPORT))
        return (NULL);
    if (new_refflag == SRF_DISABLE && (vp->flag & (ARRAY | ASSOC)) == ASSOC)
        vp->flag &= ~ASSOC;
    else if (new_refflag == SRF_ENABLE) {
        if (vp->flag & ARRAY) {
            struct tbl *a, *tmp;

            /* free up entire array */
            for (a = vp->u.array; a;) {
                tmp = a;
                a = a->u.array;
                if (tmp->flag & ALLOC)
                    afree(tmp->val.s, tmp->areap);
                afree(tmp, tmp->areap);
            }
            vp->u.array = NULL;
            vp->flag &= ~ARRAY;
        }
        vp->flag |= ASSOC;
    }

    set &= ~(LOCAL | LOCAL_COPY);

    vpbase = (vp->flag & ARRAY) ? arraybase(tvar) : vp;

    /*
     * only allow export and read-only flag to be set; AT&T ksh
     * allows any attribute to be changed which means it can be
     * truncated or modified (-L/-R/-Z/-i)
     */
    if ((vpbase->flag & RDONLY) && (val || clr || (set & ~(EXPORT | RDONLY))))
        merrf(NULL, (ep, KWF_ERR(2) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO,
                     Tread_only, tvar));
    if (tvar != tvarbuf)
        afree(tvar, ATEMP);

    /* most calls are with set/clr == 0 */
    if (set | clr) {
        bool ok = true;

        /*
         * XXX if x[0] isn't set, there will be problems: need
         * to have one copy of attributes for arrays...
         */
        for (t = vpbase; t; t = t->u.array) {
            bool fake_assign;
            const char *s = NULL;
            char *free_me = NULL;

            fake_assign = (t->flag & ISSET) && (!val || t != vp) &&
                          ((set & (UCASEV_AL | LCASEV | LJUST | RJUST | ZEROFIL)) ||
                           ((t->flag & INTEGER) && (clr & INTEGER)) ||
                           (!(t->flag & INTEGER) && (set & INTEGER)));
            if (fake_assign) {
                if (t->flag & INTEGER) {
                    s = str_val(t);
                    free_me = NULL;
                } else {
                    s = t->val.s + t->type;
                    free_me = (t->flag & ALLOC) ? t->val.s : NULL;
                }
                t->flag &= ~ALLOC;
            }
            if (!(t->flag & INTEGER) && (set & INTEGER)) {
                t->type = 0;
                t->flag &= ~ALLOC;
            }
            if (set & INTEGER) {
                /*
                 * Don't change base if assignment is to
                 * be done, in case assignment fails.
                 */
                if (base > 0 && (!val || t != vp))
                    t->type = base;
                /*
                 * Do not permit content from the
                 * environment to e.g. execute commands.
                 */
                if ((t->flag & IMPORT) && fake_assign) {
                    qsh_ari_u num;

                    if (getnum(s, &num, true, ((bool)(Flag(FPOSIX)))) == -1)
                        s = "0";
                    clr |= IMPORT;
                }
            }
            t->flag = (t->flag | set) & ~clr;
            if (set & (LJUST | RJUST | ZEROFIL))
                t->u2.field = field;
            if (fake_assign) {
                if (!setstr(t, s, QSH_RETURN_ERROR)) {
                    /*
                     * Somewhat arbitrary action
                     * here: zap contents of
                     * variable, but keep the flag
                     * settings.
                     */
                    ok = false;
                    if (t->flag & INTEGER)
                        t->flag &= ~ISSET;
                    else {
                        if (t->flag & ALLOC)
                            afree(t->val.s, t->areap);
                        t->flag &= ~(ISSET | ALLOC);
                        t->type = 0;
                    }
                }
                afree(free_me, t->areap);
            }
        }
        if (!ok)
            merrf(NULL, (ep, KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG | KWF_NOERRNO,
                         "failed to set string value"));
    }

    if (vappend) {
        size_t tlen;
        if ((vp->flag & (ISSET | ALLOC | SPECIAL | INTEGER | UCASEV_AL | LCASEV | LJUST | RJUST |
                         IMPORT)) != (ISSET | ALLOC)) {
            /* cannot special-case this */
            strdup2x(tvar, str_val(vp), val);
            val = tvar;
            goto vassign;
        }
        /* trivial string appending */
        len = strlen(vp->val.s);
        tlen = strlen(val) + 1;
        vp->val.s = aresize1(vp->val.s, len, tlen, vp->areap);
        memcpy(vp->val.s + len, val, tlen);
    } else if (val != NULL) {
    vassign:
        if (vp->flag & INTEGER) {
            /* do not zero base before assignment */
            setstr(vp, val, QSH_UNWIND_ERROR | 0x4);
            /* done after assignment to override default */
            if (base > 0)
                vp->type = base;
        } else {
            /* setstr can't fail (read-only check already done) */
            setstr(vp, val, QSH_RETURN_ERROR | 0x4);
            vp->flag |= (set & IMPORT);
        }

        /* came here from vappend? need to free temp val */
        if (vappend)
            afree(tvar, ATEMP);
    }

    /* only x[0] is ever exported, so use vpbase */
    if ((vpbase->flag & (EXPORT | INTEGER)) == EXPORT && vpbase->type == 0)
        exportprep(vpbase, (vpbase->flag & ISSET) ? vpbase->val.s : null, 0);

    return (vp);
}

/**
 * Unset a variable. The flags can be:
 * |1   = tear down entire array
 * |2   = keep attributes, only unset content
 */
void
unset(struct tbl *vp, int flags)
{
    if (vp->flag & ALLOC)
        afree(vp->val.s, vp->areap);
    if ((vp->flag & ARRAY) && (flags & 1)) {
        struct tbl *a, *tmp;

        /* free up entire array */
        for (a = vp->u.array; a;) {
            tmp = a;
            a = a->u.array;
            if (tmp->flag & ALLOC)
                afree(tmp->val.s, tmp->areap);
            afree(tmp, tmp->areap);
        }
        vp->u.array = NULL;
    }
    if (flags & 2) {
        vp->flag &= ~(ALLOC | ISSET);
        return;
    }
    /* if foo[0] is being unset, the remainder of the array is kept... */
    vp->flag &= SPECIAL | ((flags & 1) ? 0 : ARRAY | DEFINED);
    if (vp->flag & SPECIAL)
        /* responsible for 'unspecial'ing var */
        unsetspec(vp, true);
}

/*
 * Return a pointer to the first char past a legal variable name
 * (returns the argument if there is no legal name, returns a pointer to
 * the terminating NUL if whole string is legal).
 */
const char *
skip_varname(const char *s, bool aok)
{
    size_t alen;

    if (s && ctype(*s, C_ALPHX)) {
        do {
            ++s;
        } while (ctype(*s, C_ALNUX));
        if (aok && ord(*s) == ORD('[') && (alen = array_ref_len(s)))
            s += alen;
    }
    return (s);
}

/* Return a pointer to the first character past any legal variable name */
const char *
skip_wdvarname(const char *s,
               /* skip array de-reference? */
               bool aok)
{
    if (s[0] == CHAR && ctype(s[1], C_ALPHX)) {
        do {
            s += 2;
        } while (s[0] == CHAR && ctype(s[1], C_ALNUX));
        if (aok && s[0] == CHAR && ord(s[1]) == ORD('[')) {
            /* skip possible array de-reference */
            const char *p = s;
            char c;
            int depth = 0;

            while (/* CONSTCOND */ 1) {
                if (p[0] != CHAR)
                    break;
                c = p[1];
                p += 2;
                if (ord(c) == ORD('['))
                    depth++;
                else if (ord(c) == ORD(']') && --depth == 0) {
                    s = p;
                    break;
                }
            }
        }
    }
    return (s);
}

/* Check if coded string s is a variable name */
int
is_wdvarname(const char *s, bool aok)
{
    const char *p = skip_wdvarname(s, aok);

    return (p != s && p[0] == EOS);
}

/* Check if coded string s is a variable assignment */
int
is_wdvarassign(const char *s, bool needEOS)
{
    const char *p = skip_wdvarname(s, true);

    if (p == s || p[0] != CHAR)
        return (0);
    switch (ord(p[1])) {
    case ORD('='):
        return (!needEOS || p[2] == EOS);
    case ORD('+'):
        if (p[2] != CHAR || ord(p[3]) != ORD('='))
            return (0);
        return (!needEOS || p[4] == EOS);
    default:
        return (0);
    }
}

/* don’t leak internal hash table order */
static int
envsort(const void *a, const void *b)
{
    const kby *cp1 = *(const kby *const *)a;
    const kby *cp2 = *(const kby *const *)b;

    while (*cp1 == *cp2) {
        if (*cp1 == '=' || *cp1++ == '\0')
            return (0);
        ++cp2;
    }
    return ((int)asciibetical(*cp1) - (int)asciibetical(*cp2));
}

/*
 * Make the exported environment from the exported names in the dictionary.
 */
char **
makenv(void)
{
    ssize_t i;
    struct block *l;
    XPtrV denv;
    struct tbl *vp, **vpp;

    XPinit(denv, 64);
    for (l = e->loc; l != NULL; l = l->next) {
        vpp = l->vars.tbls;
        i = 1 << (l->vars.tshift);
        while (--i >= 0)
            if ((vp = *vpp++) != NULL && (vp->flag & (ISSET | EXPORT)) == (ISSET | EXPORT)) {
                struct block *l2;
                struct tbl *vp2;
                k32 h = hash(vp->name);

                /* unexport any redefined instances */
                for (l2 = l->next; l2 != NULL; l2 = l2->next) {
                    vp2 = ktsearch(&l2->vars, vp->name, h);
                    if (vp2 != NULL)
                        vp2->flag &= ~EXPORT;
                }
                if ((vp->flag & INTEGER)) {
                    /* integer to string */
                    char *val;
                    val = str_val(vp);
                    vp->flag &= ~(INTEGER | RDONLY | SPECIAL);
                    /* setstr can't fail here */
                    setstr(vp, val, QSH_RETURN_ERROR);
                }
                XPput(denv, vp->val.s);
            }
        if (l->flags & BF_STOPENV)
            break;
    }
    qsort(XPptrv(denv), XPsize(denv), sizeof(void *), envsort);
    XPput(denv, NULL);
    return ((char **)XPclose(denv));
}

/*
 * handle special variables with side effects - PATH, SECONDS.
 */

