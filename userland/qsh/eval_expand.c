/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"
#include "eval_priv.h"

/* Internal forward decls — definitions live further down this file. */
static void funsub(struct op *t);
static char *valsub(struct op *t, Area *ap);
static void globit(XString *xs, char **xpp, char *sp, XPtrV *wp, int check);
static char *homedir(char *name);

static bool
hasnonempty(const char **strv)
{
    size_t i = 0;

    while (strv[i])
        if (*strv[i++])
            return (true);
    return (false);
}

/*
 * Prepare to generate the string returned by ${} substitution.
 */
int
varsub(Expand *xp, const char *sp, const char *word,
       /* becomes qualifier type */
       unsigned int *stypep,
       /* becomes qualifier type len (=, :=, etc.) valid iff *stypep != 0 */
       int *slenp)
{
    unsigned int c;
    int state;          /* next state: XBASE, XARG, XSUB, XNULLSUB */
    unsigned int stype; /* substitution type */
    int slen = 0;
    const char *p;
    struct tbl *vp;
    bool zero_ok = false;
    int sc;
    XPtrV wv;

    if ((stype = ord(sp[0])) == '\0')
        /* Bad variable name */
        return (-1);

    xp->var = NULL;

    /* entirety of named array? */
    if ((p = cstrchr(sp, '[')) && (sc = ord(p[1])) && ord(p[2]) == ORD(']'))
        /* keep p (for ${!foo[1]} below)! */
        switch (sc) {
        case ORD('*'):
            sc = 3;
            break;
        case ORD('@'):
            sc = 7;
            break;
        default:
            /* bit2 = @, bit1 = array, bit0 = enabled */
            sc = 0;
        }
    else
        /* $* and $@ checked below */
        sc = 0;

    /*-
     * ${%var}, string width (-U: screen columns, +U: octets)
     * ${#var}, string length (-U: characters, +U: octets) or array size
     * ${!var}, variable name
     * ${*…} -> set flag for argv
     * ${@…} -> set flag for argv
     */
    if (ctype(stype, C_SUB2 | CiVAR1)) {
        switch (stype) {
        case ORD('*'):
            if (!sc)
                sc = 1;
            goto nopfx;
        case ORD('@'):
            if (!sc)
                sc = 5;
            goto nopfx;
        }
        /* varname required */
        if ((c = ord(sp[1])) == '\0') {
            if (stype == ORD('%'))
                /* $% */
                return (-1);
            /* $# or $! */
            goto nopfx;
        }
        /* can’t have any modifiers for ${#…} or ${%…} or ${!…} */
        if (*word != CSUBST)
            return (-1);
        /* check for argv past prefix */
        if (!sc)
            switch (c) {
            case ORD('*'):
                sc = 1;
                break;
            case ORD('@'):
                sc = 5;
                break;
            }
        /* skip past prefix */
        ++sp;
        /* determine result */
        switch (stype) {
        case ORD('!'):
            if (sc & 2) {
                stype = 0;
                XPinit(wv, 32);
                vp = arraybase(sp);
                do {
                    if (vp->flag & ISSET)
                        XPput(wv, shf_smprintf(Tf_lu, arrayindex(vp)));
                } while ((vp = vp->u.array));
                goto arraynames;
            }
            xp->var = global(sp);
            /* use saved p from above */
            xp->str =
                p ? shf_smprintf(Tf_sSQlu, xp->var->name, arrayindex(xp->var)) : xp->var->name;
            break;
        case ORD('%'):
            /* cannot do this on an array */
            if (sc)
                return (-1);
            p = str_val(global(sp));
            zero_ok = p != null;
            /* partial utf_mbswidth reimplementation */
            sc = 0;
            while (*p) {
                p += ez_mbtowc(&c, p);
                /* c == char or wchar at p++ */
                if ((slen = utf_wcwidth(c)) == -1) {
                    /* 646, 8859-1, 10646 C0/C1 */
                    sc = -1;
                    break;
                }
                sc += slen;
            }
            if (0)
                /* FALLTHROUGH */
            case ORD('#'):
                switch (sc & 3) {
                case 3:
                    vp = arraybase(sp);
                    if (vp->flag & (ISSET | ARRAY))
                        zero_ok = true;
                    sc = 0;
                    do {
                        if (vp->flag & ISSET)
                            sc++;
                    } while ((vp = vp->u.array));
                    break;
                case 1:
                    sc = e->loc->argc;
                    break;
                default:
                    p = str_val(global(sp));
                    zero_ok = p != null;
                    sc = utflen(p);
                    break;
                }
            /* ${%var} also here */
            if (Flag(FNOUNSET) && sc == 0 && !zero_ok)
                kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, sp,
                      Tf_parm);
            xp->str = shf_smprintf(Tf_d, sc);
            break;
        }
        /* unqualified variable/string substitution */
        *stypep = 0;
        return (XSUB);
    }
nopfx:

    /* check for qualifiers in word part */
    stype = 0;
    /*slen = 0;*/
    c = word[/*slen +*/ 0] == CHAR ? ord(word[/*slen +*/ 1]) : 0;
    if (c == ORD(':')) {
        slen += 2;
        stype = STYPE_DBL;
        c = word[slen + 0] == CHAR ? ord(word[slen + 1]) : 0;
    }
    if (!stype && c == ORD('/')) {
        slen += 2;
        stype = c;
        if (word[slen] == ADELIM && ord(word[slen + 1]) == c) {
            slen += 2;
            stype |= STYPE_DBL;
        }
    } else if (stype == STYPE_DBL && (c == ORD(' ') || c == ORD('0'))) {
        stype |= ORD('0');
    } else if (ctype(c, C_SUB1)) {
        slen += 2;
        stype |= c;
    } else if (ctype(c, C_SUB2)) {
        /* Note: ksh88 allows :%, :%%, etc */
        slen += 2;
        stype = c;
        if (word[slen + 0] == CHAR && ord(word[slen + 1]) == c) {
            stype |= STYPE_DBL;
            slen += 2;
        }
    } else if (c == ORD('@')) {
        /* @x where x is command char */
        switch (c = ord(word[slen + 2]) == CHAR ? ord(word[slen + 3]) : 0) {
        case ORD('#'):
        case ORD('/'):
        case ORD('Q'):
        case ORD('^'):
            break;
        default:
            return (-1);
        }
        stype |= STYPE_AT | c;
        slen += 4;
    } else if (stype)
        /* : is not ok */
        return (-1);
    if (!stype && *word != CSUBST)
        return (-1);

    if (!sc) {
        xp->var = global(sp);
        xp->str = str_val(xp->var);
        /* can't assign things like $! or $1 */
        if ((stype & STYPE_SINGLE) == ORD('=') && !*xp->str && ctype(*sp, C_VAR1 | C_DIGIT))
            return (-1);
        state = XSUB;
    } else {
        /* can’t assign/trim a vector (yet) */
        switch (stype & STYPE_SINGLE) {
        case ORD('-'):
        case ORD('+'):
            /* allowed ops */
        case 0:
            /* or no ops */
            break;
        /*  case ORD('='):
        case ORD('?'):
        case ORD('#'):
        case ORD('%'):
        case ORD('/'):
        case ORD('/') | STYPE_AT:
        case ORD('0'):
        case ORD('#') | STYPE_AT:
        case ORD('Q') | STYPE_AT:
        case ORD('^') | STYPE_AT:
    */
        default:
            return (-1);
        }
        /* do what we can */
        if (sc & 2) {
            XPinit(wv, 32);
            vp = arraybase(sp);
            do {
                if (vp->flag & ISSET)
                    XPput(wv, str_val(vp));
            } while ((vp = vp->u.array));
        arraynames:
            if ((c = (XPsize(wv) == 0)))
                XPfree(wv);
            else {
                XPput(wv, NULL);
                xp->u.strv = (const char **)XPptrv(wv);
            }
        } else {
            if ((c = (e->loc->argc == 0)))
                xp->var = global(sp);
            else
                xp->u.strv = (const char **)e->loc->argv + 1;
            /* POSIX 2009? */
            zero_ok = true;
        }
        /* have we got any elements? */
        if (c) {
            /* no */
            xp->str = null;
            state = sc & 4 ? XNULLSUB : XSUB;
        } else {
            /* yes → load first */
            xp->str = *xp->u.strv++;
            /* $@ or ${foo[@]} */
            xp->split = ((bool)(sc & 4));
            state = XARG;
        }
    }

    c = stype & STYPE_CHAR;
    /* test the compiler's code generator */
    if ((!(stype & STYPE_AT) &&
         (ctype(c, C_SUB2) ||
          (((stype & STYPE_DBL) ? *xp->str == '\0' : xp->str == null) &&
                   (state != XARG ||
                    (ifs0 || xp->split ? (xp->u.strv[0] == NULL) : !hasnonempty(xp->u.strv)))
               ? ctype(c, C_EQUAL | C_MINUS | C_QUEST)
               : c == ORD('+')))) ||
        stype == (ORD('0') | STYPE_DBL) || stype == (ORD('#') | STYPE_AT) ||
        stype == (ORD('Q') | STYPE_AT) || stype == (ORD('^') | STYPE_AT) ||
        (stype & STYPE_CHAR) == ORD('/'))
        /* expand word instead of variable value */
        state = XBASE;
    if (Flag(FNOUNSET) && xp->str == null && !zero_ok &&
        (ctype(c, C_SUB2) || (state != XBASE && c != ORD('+'))))
        kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, sp, Tf_parm);
    *stypep = stype;
    *slenp = slen;
    return (state);
}

/*
 * Run the command in $(...) and read its output.
 */
int
comsub(Expand *xp, const char *cp, int fn)
{
    Source *s, *sold;
    struct op *t;
    struct shf *shf;
    bool doalias = false;
    kby old_utfmode = UTFMODE;

    switch (fn) {
    case COMASUB:
        fn = COMSUB;
        if (0)
            /* FALLTHROUGH */
        case FUNASUB:
            fn = FUNSUB;
        doalias = true;
    }

    s = pushs(SSTRING, ATEMP);
    s->start = s->str = cp;
    sold = source;
    t = compile(s, doalias);
    afree(s, ATEMP);
    source = sold;

    UTFMODE = old_utfmode;

    if (t == NULL)
        return (XBASE);

    /* no waitlast() unless specifically enabled later */
    xp->split = false;

    if (t->type == TCOM && *t->args == NULL && *t->vars == NULL && t->ioact != NULL) {
        /* $(<file) */
        struct ioword *io = *t->ioact;
        char *name;

        switch (io->ioflag & IOTYPE) {
        case IOREAD:
            shf =
                shf_open(name = evalstr(io->ioname, DOTILDE), O_RDONLY, 0, SHF_MAPHI | SHF_CLEXEC);
            if (shf == NULL)
                kwarnf(KWF_PREFIX | (Flag(FTALKING) ? KWF_FILELINE : 0) | KWF_TWOMSG, name,
                       Tcant_filesub);
            break;
        case IOHERE:
            if (!herein(io, &name)) {
                xp->str = name;
                /* as $(…) requires, trim trailing newlines */
                name = strnul(name);
                while (name > xp->str && name[-1] == '\n')
                    --name;
                *name = '\0';
                return (XSUB);
            }
            shf = NULL;
            break;
        default:
            kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO,
                  T_funny_command, snptreef(NULL, 32, Tft_R, io));
        }
    } else if (fn == FUNSUB) {
        int ofd1;
        struct temp *tf = NULL;

        /*
         * create a temporary file, open for reading and writing,
         * with an shf open for reading (buffered) but yet unused
         */
        maketemp(ATEMP, TT_FUNSUB, &tf);
        if (!tf->shf)
            kerrf0(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE, Tf_temp, Tcreate, tf->tffn);
        /* extract shf from temporary file, unlink and free it */
        shf = tf->shf;
        unlink(tf->tffn);
        afree(tf, ATEMP);
        /* save stdout and let it point to the tempfile */
        ofd1 = savefd(1);
        qsh_dup2(shf_fileno(shf), 1, false);
        /*
         * run tree, with output thrown into the tempfile,
         * in a new function block
         */
        funsub(t);
        subst_exstat = exstat & 0xFF;
        /* rewind the tempfile and restore regular stdout */
        lseek(shf_fileno(shf), (off_t)0, SEEK_SET);
        restfd(1, ofd1);
    } else if (fn == VALSUB) {
        xp->str = valsub(t, ATEMP);
        subst_exstat = exstat & 0xFF;
        return (XSUB);
    } else {
        int ofd1, pv[2];

        openpipe(pv);
        shf = shf_fdopen(pv[0], SHF_RD, NULL);
        ofd1 = savefd(1);
        if (pv[1] != 1) {
            qsh_dup2(pv[1], 1, false);
            close(pv[1]);
        }
        execute(t, XXCOM | XPIPEO | XFORK, NULL);
        restfd(1, ofd1);
        startlast();
        /* waitlast() */
        xp->split = true;
    }

    xp->u.shf = shf;
    return (XCOM);
}

/*
 * perform #pattern and %pattern substitution in ${}
 */
char *
trimsub(char *str, char *pat, int how)
{
    char *end = strnul(str);
    char *p, c;

    switch (how & (STYPE_CHAR | STYPE_DBL)) {
    case ORD('#'):
        /* shortest match at beginning */
        p = str;
        do {
            c = *p;
            *p = '\0';
            if (gmatchx(str, pat, false)) {
                record_match(str);
                *p = c;
                return (p);
            }
            *p = c;
            p += ez_mbtoc(NULL, p);
        } while (c != '\0');
        break;
    case ORD('#') | STYPE_DBL:
        /* longest match at beginning */
        p = end;
        while (p >= str) {
            c = *p;
            *p = '\0';
            if (gmatchx(str, pat, false)) {
                record_match(str);
                *p = c;
                return (p);
            }
            *p = c;
            --p;
        }
        break;
    case ORD('%'):
        /* shortest match at end */
        p = end;
        while (p >= str) {
            if (gmatchx(p, pat, false))
                goto trimsub_match;
            p = ez_bs(p - 1, str);
        }
        break;
    case ORD('%') | STYPE_DBL:
        /* longest match at end */
        for (p = str; p <= end; p++)
            if (gmatchx(p, pat, false)) {
            trimsub_match:
                record_match(p);
                strndupx(end, str, p - str, ATEMP);
                return (end);
            }
        break;
    }

    /* no match, return string */
    return (str);
}

/*
 * glob
 * Name derived from V6's /etc/glob, the program that expanded filenames.
 */

/* XXX cp not const 'cause slashes are temporarily replaced with NULs... */
void
glob(char *cp, XPtrV *wp, bool markdirs)
{
    int oldsize = XPsize(*wp);

    if (glob_str(cp, wp, markdirs) == 0)
        XPput(*wp, debunk(cp, cp, strlen(cp) + 1));
    else
        qsort(XPptrv(*wp) + oldsize, XPsize(*wp) - oldsize, sizeof(void *), ascpstrcmp);
}

#define GF_NONE 0
#define GF_EXCHECK BIT(0) /* do existence check on file */
#define GF_GLOBBED BIT(1) /* some globbing has been done */
#define GF_MARKDIR BIT(2) /* add trailing / to directories */

/*
 * Apply file globbing to cp and store the matching files in wp. Returns
 * the number of matches found.
 */
int
glob_str(char *cp, XPtrV *wp, bool markdirs)
{
    int oldsize = XPsize(*wp);
    XString xs;
    char *xp;

    Xinit(xs, xp, 256, ATEMP);
    globit(&xs, &xp, cp, wp, markdirs ? GF_MARKDIR : GF_NONE);
    Xfree(xs, xp);

    return (XPsize(*wp) - oldsize);
}

static void
globit(XString *xs, /* dest string */
       char **xpp,  /* ptr to dest end */
       char *sp,    /* source path */
       XPtrV *wp,   /* output list */
       int check)   /* GF_* flags */
{
    char *np; /* next source component */
    char *xp = *xpp;
    char *se;
    char odirsep;
    DIR *dirp;
    size_t prefix_len;

    /* This to allow long expansions to be interrupted */
    intrcheck();

    if (sp == NULL) {
        /* end of source path */
        /*
         * We only need to check if the file exists if a pattern
         * is followed by a non-pattern (eg, foo*x/bar; no check
         * is needed for foo* since the match must exist) or if
         * any patterns were expanded and the markdirs option is set.
         * Symlinks make things a bit tricky...
         */
        if ((check & GF_EXCHECK) || ((check & GF_MARKDIR) && (check & GF_GLOBBED))) {
#define stat_check()                                                                               \
    (stat_done ? stat_done : (stat_done = stat(Xstring(*xs, xp), &statb) < 0 ? -1 : 1))
            struct stat lstatb, statb;
            /* -1: failed, 1 ok, 0 not yet done */
            int stat_done = 0;

            if (qsh_lstat(Xstring(*xs, xp), &lstatb) < 0)
                return;
            /*
             * special case for systems which strip trailing
             * slashes from regular files (eg, /etc/passwd/).
             * SunOS 4.1.3 does this...
             */
            if ((check & GF_EXCHECK) && xp > Xstring(*xs, xp) && qsh_cdirsep(xp[-1]) &&
                !S_ISDIR(lstatb.st_mode) &&
                (!S_ISLNK(lstatb.st_mode) || stat_check() < 0 || !S_ISDIR(statb.st_mode)))
                return;
            /*
             * Possibly tack on a trailing / if there isn't already
             * one and if the file is a directory or a symlink to a
             * directory
             */
            if (((check & GF_MARKDIR) && (check & GF_GLOBBED)) && xp > Xstring(*xs, xp) &&
                !qsh_cdirsep(xp[-1]) &&
                (S_ISDIR(lstatb.st_mode) ||
                 (S_ISLNK(lstatb.st_mode) && stat_check() > 0 && S_ISDIR(statb.st_mode)))) {
                *xp++ = '/';
                *xp = '\0';
            }
        }
        strndupx(np, Xstring(*xs, xp), Xlength(*xs, xp), ATEMP);
        XPput(*wp, np);
        return;
    }

    if (xp > Xstring(*xs, xp))
        *xp++ = '/';
    while (qsh_cdirsep(*sp)) {
        Xcheck(*xs, xp);
        *xp++ = *sp++;
    }
    *xp = '\0';
    np = qsh_sdirsep(sp);
    if (np != NULL) {
        se = np;
        /* don't assume '/', can be multiple kinds */
        odirsep = *np;
        *np++ = '\0';
    } else {
        odirsep = '\0'; /* keep gcc quiet */
        se = strnul(sp);
    }

    /*
     * Check if sp needs globbing - done to avoid pattern checks for strings
     * containing MAGIC characters, open [s without the matching close ],
     * etc. (otherwise opendir() will be called which may fail because the
     * directory isn't readable - if no globbing is needed, only execute
     * permission should be required (as per POSIX)).
     */
    if (!has_globbing(sp)) {
        XcheckN(*xs, xp, se - sp + 1);
        debunk(xp, sp, Xnleft(*xs, xp));
        xp = strnul(xp);
        *xpp = xp;
        globit(xs, xpp, np, wp, check);
    } else if ((dirp = opendir((prefix_len = Xlength(*xs, xp)) ? Xstring(*xs, xp) : Tdot))) {
        struct dirent *d;

        while ((d = readdir(dirp)) != NULL) {
            size_t len;

            if (isch(d->d_name[0], '.') &&
                (!d->d_name[1] || (isch(d->d_name[1], '.') && !d->d_name[2])))
                /* always ignore . and .. */
                continue;
            if ((isch(d->d_name[0], '.') && !isch(*sp, '.')) || !gmatchx(d->d_name, sp, true))
                continue;

            len = strlen(d->d_name) + 1;
            XcheckN(*xs, xp, len);
            memcpy(xp, d->d_name, len);
            *xpp = xp + len - 1;
            globit(xs, xpp, np, wp,
                   (check & GF_MARKDIR) | GF_GLOBBED | (np ? GF_EXCHECK : GF_NONE));
            xp = Xstring(*xs, xp) + prefix_len;
        }
        closedir(dirp);
    }

    if (np != NULL)
        *(char *)(--np) = odirsep;
}

/* remove MAGIC from string */
char *
debunk(char *dp, const char *sp, size_t dlen)
{
    char *d;
    const char *s;

    if ((s = cstrchr(sp, MAGIC))) {
        if (s - sp >= (ssize_t)dlen)
            return (dp);
        memmove(dp, sp, s - sp);
        for (d = dp + (s - sp); *s && (d - dp < (ssize_t)dlen); s++)
            if (!ISMAGIC(*s) || !(*++s & 0x80) || !ctype(*s & 0x7F, C_PATMO | C_SPC))
                *d++ = *s;
            else {
                /* extended pattern operators: *+?@! */
                if ((*s & 0x7f) != ' ')
                    *d++ = *s & 0x7f;
                if (d - dp < (ssize_t)dlen)
                    *d++ = '(';
            }
        *d = '\0';
    } else if (dp != sp)
        strlcpy(dp, sp, dlen);
    return (dp);
}

/*
 * Check if p is an unquoted name, possibly followed by a / or :. If so
 * puts the expanded version in *dcp,dp and returns a pointer in p just
 * past the name, otherwise returns 0.
 */
const char *
maybe_expand_tilde(const char *p, XString *dsp, char **dpp, bool isassign)
{
    XString ts;
    char *dp = *dpp;
    char *tp;
    const char *r;

    Xinit(ts, tp, 16, ATEMP);
    /* : only for DOASNTILDE form */
    while (p[0] == CHAR && /* not cdirsep */ p[1] != '/' && (!isassign || p[1] != ':')) {
        Xcheck(ts, tp);
        *tp++ = p[1];
        p += 2;
    }
    *tp = '\0';
    r = (p[0] == EOS || p[0] == CHAR || p[0] == CSUBST) ? do_tilde(Xstring(ts, tp)) : NULL;
    Xfree(ts, tp);
    if (r) {
        while (*r) {
            Xcheck(*dsp, dp);
            if (ISMAGIC(*r))
                *dp++ = MAGIC;
            *dp++ = *r++;
        }
        *dpp = dp;
        r = p;
    }
    return (r);
}

/*
 * tilde expansion
 *
 * based on a version by Arnold Robbins
 */
char *
do_tilde(char *cp)
{
    char *dp = null;

    if (cp[0] == '\0')
        dp = str_val(global("HOME"));
    else if (cp[0] == '+' && cp[1] == '\0')
        dp = str_val(global(TPWD));
    else if (qsh_isdash(cp))
        dp = str_val(global(TOLDPWD));

    /* if parameters aren't set, don't expand ~ */
    if (dp == NULL || dp == null)
        return (NULL);

    /* simplify parameters as if cwd upon entry */
    {
        strdupx(dp, dp, ATEMP);
        simplify_path(dp);
    }
    return (dp);
}

void
alt_expand(XPtrV *wp, char *start, char *exp_start, char *end, int fdo)
{
    unsigned int count = 0;
    char *brace_start, *brace_end, *comma = NULL;
    char *field_start;
    char *p = exp_start;

    /* search for open brace */
    while ((p = ucstrchr(p, MAGIC)) && ord(p[1]) != ORD('{' /*}*/))
        p += 2;
    brace_start = p;

    /* find matching close brace, if any */
    if (p) {
        comma = NULL;
        count = 1;
        p += 2;
        while (*p && count) {
            if (ISMAGIC(*p++)) {
                if (ord(*p) == ORD('{' /*}*/))
                    ++count;
                else if (ord(*p) == ORD(/*{*/ '}'))
                    --count;
                else if (*p == ',' && count == 1)
                    comma = p;
                ++p;
            }
        }
    }
    /* no valid expansions... */
    if (!p || count != 0) {
        /*
         * Note that given a{{b,c} we do not expand anything (this is
         * what AT&T ksh does. This may be changed to do the {b,c}
         * expansion. }
         */
        if (fdo & DOGLOB)
            glob(start, wp, ((bool)(fdo & DOMARKDIRS)));
        else
            XPput(*wp, debunk(start, start, end - start));
        return;
    }
    brace_end = p;
    if (!comma) {
        alt_expand(wp, start, brace_end, end, fdo);
        return;
    }

    /* expand expression */
    field_start = brace_start + 2;
    count = 1;
    for (p = brace_start + 2; p != brace_end; p++) {
        if (ISMAGIC(*p)) {
            if (ord(*++p) == ORD('{' /*}*/))
                ++count;
            else if ((ord(*p) == ORD(/*{*/ '}') && --count == 0) || (*p == ',' && count == 1)) {
                char *news;
                size_t l1, l2, l3;

                /*
                 * addition safe since these operate on
                 * one string (separate substrings)
                 */
                l1 = brace_start - start;
                l2 = (p - 1) - field_start;
                l3 = end - brace_end;
                news = alloc(l1 + l2 + l3 + 1, ATEMP);
                memcpy(news, start, l1);
                memcpy(news + l1, field_start, l2);
                memcpy(news + l1 + l2, brace_end, l3);
                news[l1 + l2 + l3] = '\0';
                alt_expand(wp, news, news + l1, news + l1 + l2 + l3, fdo);
                field_start = p + 1;
            }
        }
    }
    return;
}

static void
subcpybk(struct block *l)
{
    /* did the valsub/funsub change $#/$*? */
    if (e->loc->argc == l->argc && e->loc->argv == l->argv)
        return;

    /* yes ⇒ copy back to parent Area */
    l->argv = cpyargv(&l->argc, e->loc->argv, &l->area);
}

/* helper function due to setjmp/longjmp woes */
static char *
valsub(struct op *t, Area *ap)
{
    char *cp;
    struct tbl *volatile vp;
    struct block *volatile l;
    int i;

    l = e->loc;
    newenv(E_FUNC);
    newblock();
    vp = local(TREPLY, false);
    if (!(i = qshsetjmp(e->jbuf))) {
        execute(t, XXCOM, NULL);
        i = LRETURN;
    }
    strdupx(cp, str_val(vp), ap);
    subcpybk(l);
    quitenv(NULL);
    /* see CFUNC case in exec.c:comexec() */
    if (i != LRETURN)
        unwind(i);

    return (cp);
}

static void
funsub(struct op *t)
{
    struct block *volatile l;

    l = e->loc;
    newenv(E_FUNC);
    newblock();
    if (!qshsetjmp(e->jbuf))
        execute(t, XXCOM | XERROK, NULL);
    subcpybk(l);
    quitenv(NULL);
}
