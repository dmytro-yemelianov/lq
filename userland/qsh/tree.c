/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

#define INDENT 8

static void ptree(struct op *, int, struct shf *);
static void pioact(struct shf *, struct ioword *);
static const char *wdvarput(struct shf *, const char *, int, int);
static void vfptreef(struct shf *, int, const char *, va_list);
static struct ioword **iocopy(struct ioword **, Area *);
static void iofree(struct ioword **, Area *);

/* "foo& ; bar" and "foo |& ; bar" are invalid */
static bool prevent_semicolon;

/* here document diversion */
static unsigned short ptree_nest;
static bool ptree_hashere;
static struct shf ptree_heredoc;
#define ptree_outhere(shf)                                                                         \
    do {                                                                                           \
        if (ptree_hashere) {                                                                       \
            char *ptree_thehere;                                                                   \
                                                                                                   \
            ptree_thehere = shf_sclose(&ptree_heredoc);                                            \
            shf_puts(ptree_thehere, (shf));                                                        \
            shf_putc('\n', (shf));                                                                 \
            afree(ptree_thehere, ATEMP);                                                           \
            ptree_hashere = false;                                                                   \
            /*prevent_semicolon = true;*/                                                            \
        }                                                                                          \
    } while (/* CONSTCOND */ 0)

static const char Telif_pT[] = "elif %T";

/*
 * print a command tree
 */
static void
ptree(struct op *t, int indent, struct shf *shf)
{
    const char **w;
    struct ioword **ioact;
    struct op *t1;
    int i;
    const char *ccp;

Chain:
    if (t == NULL)
        return;
    switch (t->type) {
    case TCOM:
        prevent_semicolon = false;
        /* special-case 'var=<<EOF' (cf. exec.c:execute) */
        if (t->args &&
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
            fptreef(shf, indent, Tf_S, t->vars[0]);
            break;
        }

        if (t->vars) {
            w = (const char **)t->vars;
            while (*w)
                fptreef(shf, indent, Tf_S_, *w++);
        }
        else
            shf_puts("#no-vars# ", shf);
        if (t->args) {
            w = t->args;
            if (*w && **w == CHAR) {
                char *cp = wdstrip(*w++, WDS_TPUTS);

                if (valid_alias_name(cp))
                    shf_putc('\\', shf);
                shf_puts(cp, shf);
                shf_putc(' ', shf);
                afree(cp, ATEMP);
            }
            while (*w)
                fptreef(shf, indent, Tf_S_, *w++);
        }
        else
            shf_puts("#no-args# ", shf);
        break;
    case TEXEC:
        t = t->left;
        goto Chain;
    case TPAREN:
        fptreef(shf, indent + 2, "( %T) ", t->left);
        break;
    case TPIPE:
        fptreef(shf, indent, "%T| ", t->left);
        t = t->right;
        goto Chain;
    case TLIST:
        fptreef(shf, indent, "%T%;", t->left);
        t = t->right;
        goto Chain;
    case TOR:
    case TAND:
        fptreef(shf, indent, "%T%s %T", t->left, (t->type == TOR) ? "||" : "&&", t->right);
        break;
    case TBANG:
        shf_puts("! ", shf);
        prevent_semicolon = false;
        t = t->right;
        goto Chain;
    case TDBRACKET:
        w = t->args;
        shf_puts("[[", shf);
        while (*w)
            fptreef(shf, indent, Tf__S, *w++);
        shf_puts(" ]] ", shf);
        break;
    case TSELECT:
    case TFOR:
        fptreef(shf, indent, "%s %s ", (t->type == TFOR) ? "for" : Tselect, t->str);
        if (t->vars != NULL) {
            shf_puts("in ", shf);
            w = (const char **)t->vars;
            while (*w)
                fptreef(shf, indent, Tf_S_, *w++);
            fptreef(shf, indent, Tft_end);
        }
        fptreef(shf, indent + INDENT, "do%N%T", t->left);
        fptreef(shf, indent, "%;done ");
        break;
    case TCASE:
        fptreef(shf, indent, "case %S in", t->str);
        for (t1 = t->left; t1 != NULL; t1 = t1->right) {
            fptreef(shf, indent, "%N(");
            w = (const char **)t1->vars;
            while (*w) {
                fptreef(shf, indent, "%S%c", *w, (w[1] != NULL) ? '|' : ')');
                ++w;
            }
            fptreef(shf, indent + INDENT, "%N%T%N;%c", t1->left, t1->u.charflag);
        }
        fptreef(shf, indent, "%Nesac ");
        break;
    case TELIF:
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_ONEMSG | KWF_NOERRNO, TELIF_unexpected);
        /* FALLTHROUGH */
    case TIF:
        i = 2;
        t1 = t;
        goto process_TIF;
        do {
            t1 = t1->right;
            i = 0;
            fptreef(shf, indent, Tft_end);
        process_TIF:
            /* 5 == strlen("elif ") */
            fptreef(shf, indent + 5 - i, Telif_pT + i, t1->left);
            t1 = t1->right;
            if (t1->left != NULL) {
                fptreef(shf, indent, Tft_end);
                fptreef(shf, indent + INDENT, "%s%N%T", "then", t1->left);
            }
        } while (t1->right && t1->right->type == TELIF);
        if (t1->right != NULL) {
            fptreef(shf, indent, Tft_end);
            fptreef(shf, indent + INDENT, "%s%N%T", "else", t1->right);
        }
        fptreef(shf, indent, "%;fi ");
        break;
    case TWHILE:
    case TUNTIL:
        /* 6 == strlen("while "/"until ") */
        fptreef(shf, indent + 6, Tf_s_T, (t->type == TWHILE) ? "while" : "until", t->left);
        fptreef(shf, indent, Tft_end);
        fptreef(shf, indent + INDENT, "do%N%T", t->right);
        fptreef(shf, indent, "%;done ");
        break;
    case TBRACE:
        fptreef(shf, indent + INDENT, "{%N%T", t->left);
        fptreef(shf, indent, "%;} ");
        break;
    case TCOPROC:
        fptreef(shf, indent, "%T|& ", t->left);
        prevent_semicolon = true;
        break;
    case TASYNC:
        fptreef(shf, indent, "%T& ", t->left);
        prevent_semicolon = true;
        break;
    case TFUNCT:
        fpFUNCTf(shf, indent, ((bool)(t->u.qsh_func)), t->str, t->left);
        break;
    case TTIME:
        fptreef(shf, indent, Tf_s_T, Ttime, t->left);
        break;
    default:
        shf_puts("<botch>", shf);
        prevent_semicolon = false;
        break;
    }
    if ((ioact = t->ioact) != NULL)
        while (*ioact != NULL)
            pioact(shf, *ioact++);
}

static void
pioact(struct shf *shf, struct ioword *iop)
{
    unsigned short flag = iop->ioflag;
    unsigned short type = flag & IOTYPE;
    short expected;

    expected = (type == IOREAD || type == IORDWR || type == IOHERE) ? 0
               : (type == IOCAT || type == IOWRITE)                 ? 1
               : (type == IODUP && (iop->unit == !(flag & IORDUP))) ? iop->unit
                                                                    : iop->unit + 1;
    if (iop->unit != expected)
        shf_fprintf(shf, Tf_d, (int)iop->unit);

    switch (type) {
    case IOREAD:
        shf_putc('<', shf);
        break;
    case IOHERE:
        if (flag & IOHERESTR) {
            shf_puts("<<<", shf);
            goto ioheredelim;
        }
        shf_puts("<<", shf);
        if (flag & IOSKIP)
            shf_putc('-', shf);
        if (iop->heredoc /* nil when tracing */) {
            /* here document diversion */
            if (!ptree_hashere) {
                shf_sopen(NULL, 0, SHF_WR | SHF_DYNAMIC, &ptree_heredoc);
                ptree_hashere = true;
            }
            shf_putc('\n', &ptree_heredoc);
            shf_puts(iop->heredoc, &ptree_heredoc);
            /* iop->delim is set before iop->heredoc */
            shf_putsv(evalstr(iop->delim, 0), &ptree_heredoc);
        }
    ioheredelim:
        /* delim is NULL during syntax error printing */
        if (iop->delim && !(iop->ioflag & IONDELIM))
            wdvarput(shf, iop->delim, 0, WDS_TPUTS);
        break;
    case IOCAT:
        shf_puts(">>", shf);
        break;
    case IOWRITE:
        shf_putc('>', shf);
        if (flag & IOCLOB)
            shf_putc('|', shf);
        break;
    case IORDWR:
        shf_puts("<>", shf);
        break;
    case IODUP:
        shf_putc(flag & IORDUP ? '<' : '>', shf);
        shf_putc('&', shf);
        break;
    }
    /* name is NULL for IOHERE or when printing syntax errors */
    if (iop->ioname) {
        if (flag & IONAMEXP)
            print_value_quoted(shf, iop->ioname);
        else
            wdvarput(shf, iop->ioname, 0, WDS_TPUTS);
    }
    shf_putc(' ', shf);
    prevent_semicolon = false;
}

/* variant of fputs for ptreef and wdstrip */
static const char *
wdvarput(struct shf *shf, const char *wp, int quotelevel, int opmode)
{
    int c;
    const char *cs;

    /*-
     * problems:
     *  `...` -> $(...)
     *  'foo' -> "foo"
     *  x${foo:-"hi"} -> x${foo:-hi} unless WDS_TPUTS
     *  x${foo:-'hi'} -> x${foo:-hi}
     * could change encoding to:
     *  OQUOTE ["'] ... CQUOTE ["']
     *  COMSUB [(`] ...\0   (handle $ ` \ and maybe " in `...` case)
     */
    while (/* CONSTCOND */ 1)
        switch (*wp++) {
        case EOS:
            return (--wp);
        case ADELIM:
            if (ord(*wp) == ORD(/*{*/ '}')) {
                ++wp;
                goto wdvarput_csubst;
            }
            /* FALLTHROUGH */
        case CHAR:
            c = ord(*wp++);
            shf_putc(c, shf);
            break;
        case QCHAR:
            c = ord(*wp++);
            if (opmode & WDS_TPUTS)
                switch (c) {
                default:
                    if (quotelevel == 0)
                        /* FALLTHROUGH */
                    case ORD('"'):
                    case ORD('`'):
                    case ORD('$'):
                    case ORD('\\'):
                        shf_putc(ORD('\\'), shf);
                    break;
                }
            shf_putc(c, shf);
            break;
        case COMASUB:
        case COMSUB:
            shf_puts("$(", shf);
            cs = ")";
            if (ord(*wp) == ORD('(' /*)*/))
                shf_putc(' ', shf);
        pSUB:
            while ((c = *wp++) != 0)
                shf_putc(c, shf);
            shf_puts(cs, shf);
            break;
        case FUNASUB:
        case FUNSUB:
            c = ORD(' ');
            if (0)
                /* FALLTHROUGH */
            case VALSUB:
                c = ORD('|');
            shf_putc('$', shf);
            shf_putc('{', shf);
            shf_putc(c, shf);
            cs = ";}";
            goto pSUB;
        case EXPRSUB:
            shf_puts("$((", shf);
            cs = "))";
            goto pSUB;
        case OQUOTE:
            if (opmode & WDS_TPUTS) {
                quotelevel++;
                shf_putc('"', shf);
            }
            break;
        case CQUOTE:
            if (opmode & WDS_TPUTS) {
                if (quotelevel)
                    quotelevel--;
                shf_putc('"', shf);
            }
            break;
        case OSUBST:
            shf_putc('$', shf);
            if (ord(*wp++) == ORD('{'))
                shf_putc('{', shf);
            while ((c = *wp++) != 0)
                shf_putc(c, shf);
            wp = wdvarput(shf, wp, 0, opmode);
            break;
        case CSUBST:
            if (ord(*wp++) == ORD('}')) {
            wdvarput_csubst:
                shf_putc('}', shf);
            }
            return (wp);
        case OPAT:
            c = *wp++;
            shf_putc(c, shf);
            shf_putc('(', shf);
            break;
        case SPAT:
            c = ORD('|');
            if (0)
                /* FALLTHROUGH */
            case CPAT:
                c = ORD(/*(*/ ')');
            shf_putc(c, shf);
            break;
        }
}

/*
 * this is the _only_ way to reliably handle
 * variable args with an ANSI compiler
 */
/* VARARGS */
void
fptreef(struct shf *shf, int indent, const char *fmt, ...)
{
    va_list va;

    va_start(va, fmt);
    vfptreef(shf, indent, fmt, va);
    va_end(va);
}

/* VARARGS */
char *
snptreef(char *s, ssize_t n, const char *fmt, ...)
{
    va_list va;
    struct shf shf;

    shf_sopen(s, n, SHF_WR | (s ? 0 : SHF_DYNAMIC), &shf);

    va_start(va, fmt);
    vfptreef(&shf, 0, fmt, va);
    va_end(va);

    /* shf_sclose NUL terminates */
    return (shf_sclose(&shf));
}

static void
vfptreef(struct shf *shf, int indent, const char *fmt, va_list va)
{
    int c;

    if (!ptree_nest++)
        ptree_hashere = false;

    while ((c = ord(*fmt++))) {
        if (c == '%') {
            switch ((c = ord(*fmt++))) {
            case ORD('s'):
                /* string */
                shf_putsv(va_arg(va, char *), shf);
                break;
            case ORD('S'):
                /* word */
                wdvarput(shf, va_arg(va, char *), 0, WDS_TPUTS);
                break;
            case ORD('d'):
                /* signed decimal */
                shf_fprintf(shf, Tf_d, va_arg(va, int));
                break;
            case ORD('u'):
                /* unsigned decimal */
                shf_fprintf(shf, "%u", va_arg(va, unsigned int));
                break;
            case ORD('T'):
                /* format tree */
                ptree(va_arg(va, struct op *), indent, shf);
                goto dont_trash_prevent_semicolon;
            case ORD(';'):
                /* newline or ; */
            case ORD('N'):
                /* newline or space */
                if (shf->flags & SHF_STRING) {
                    if ((unsigned int)c == ORD(';') && !prevent_semicolon)
                        shf_putc(';', shf);
                    shf_putc(' ', shf);
                } else {
                    int i = indent;

                    ptree_outhere(shf);
                    shf_putc('\n', shf);
                    while (i >= 8) {
                        shf_putc('\t', shf);
                        i -= 8;
                    }
                    while (i--)
                        shf_putc(' ', shf);
                }
                break;
            case ORD('R'):
                /* I/O redirection */
                pioact(shf, va_arg(va, struct ioword *));
                break;
            case ORD('c'):
                /* character (octet, probably) */
                c = va_arg(va, int);
                /* FALLTHROUGH */
            default:
                shf_putc(c, shf);
                break;
            }
        } else
            shf_putc(c, shf);
        prevent_semicolon = false;
    dont_trash_prevent_semicolon:;
    }

    if (!--ptree_nest)
        ptree_outhere(shf);
}

/*
 * copy tree (for function definition)
 */
struct op *
tcopy(struct op *t, Area *ap)
{
    struct op *r;
    const char **tw;
    char **rw;

    if (t == NULL)
        return (NULL);

    r = alloc(sizeof(struct op), ap);

    r->type = t->type;
    r->u.evalflags = t->u.evalflags;

    if (t->type == TCASE)
        r->str = wdcopy(t->str, ap);
    else
        strdupx(r->str, t->str, ap);

    if (t->vars == NULL)
        r->vars = NULL;
    else {
        tw = (const char **)t->vars;
        while (*tw)
            ++tw;
        rw = r->vars = alloc2(tw - (const char **)t->vars + 1, sizeof(*tw), ap);
        tw = (const char **)t->vars;
        while (*tw)
            *rw++ = wdcopy(*tw++, ap);
        *rw = NULL;
    }

    if (t->args == NULL)
        r->args = NULL;
    else {
        tw = t->args;
        while (*tw)
            ++tw;
        r->args = (const char **)(rw = alloc2(tw - t->args + 1, sizeof(*tw), ap));
        tw = t->args;
        while (*tw)
            *rw++ = wdcopy(*tw++, ap);
        *rw = NULL;
    }

    r->ioact = (t->ioact == NULL) ? NULL : iocopy(t->ioact, ap);

    r->left = tcopy(t->left, ap);
    r->right = tcopy(t->right, ap);
    r->lineno = t->lineno;

    return (r);
}

char *
wdcopy(const char *wp, Area *ap)
{
    size_t len;

    len = wdscan(wp, EOS) - wp;
    return (memcpy(alloc(len, ap), wp, len));
}

/* return the position of prefix c in wp plus 1 */
const char *
wdscan(const char *wp, int c)
{
    int nest = 0;

    while (/* CONSTCOND */ 1)
        switch (*wp++) {
        case EOS:
            return (wp);
        case ADELIM:
            if (c == ADELIM && nest == 0)
                return (wp + 1);
            if (ord(*wp) == ORD(/*{*/ '}'))
                goto wdscan_csubst;
            /* FALLTHROUGH */
        case CHAR:
        case QCHAR:
            wp++;
            break;
        case COMASUB:
        case COMSUB:
        case FUNASUB:
        case FUNSUB:
        case VALSUB:
        case EXPRSUB:
            while (*wp++ != 0)
                ;
            break;
        case OQUOTE:
        case CQUOTE:
            break;
        case OSUBST:
            nest++;
            while (*wp++ != '\0')
                ;
            break;
        case CSUBST:
        wdscan_csubst:
            wp++;
            if (c == CSUBST && nest == 0)
                return (wp);
            nest--;
            break;
        case OPAT:
            nest++;
            wp++;
            break;
        case SPAT:
        case CPAT:
            if (c == wp[-1] && nest == 0)
                return (wp);
            if (wp[-1] == CPAT)
                nest--;
            break;
        default:
            kwarnf0(KWF_INTERNAL | KWF_WARNING | KWF_NOERRNO,
                    "wdscan: unknown char 0x%X (carrying on)", KBI(wp[-1]));
        }
}

/*
 * return a copy of wp without any of the mark up characters and with
 * quote characters (" ' \) stripped. (string is allocated from ATEMP)
 */
char *
wdstrip(const char *wp, int opmode)
{
    struct shf shf;

    shf_sopen(NULL, 32, SHF_WR | SHF_DYNAMIC, &shf);
    wdvarput(&shf, wp, 0, opmode);
    /* shf_sclose NUL terminates */
    return (shf_sclose(&shf));
}

static struct ioword **
iocopy(struct ioword **iow, Area *ap)
{
    struct ioword **ior;
    int i;

    ior = iow;
    while (*ior)
        ++ior;
    ior = alloc2(ior - iow + 1, sizeof(struct ioword *), ap);

    for (i = 0; iow[i] != NULL; i++) {
        struct ioword *p, *q;

        p = iow[i];
        q = alloc(sizeof(struct ioword), ap);
        ior[i] = q;
        *q = *p;
        if (p->ioname != NULL)
            q->ioname = wdcopy(p->ioname, ap);
        if (p->delim != NULL)
            q->delim = wdcopy(p->delim, ap);
        if (p->heredoc != NULL)
            strdupx(q->heredoc, p->heredoc, ap);
    }
    ior[i] = NULL;

    return (ior);
}

/*
 * free tree (for function definition)
 */
void
tfree(struct op *t, Area *ap)
{
    char **w;

    if (t == NULL)
        return;

    afree(t->str, ap);

    if (t->vars != NULL) {
        for (w = t->vars; *w != NULL; w++)
            afree(*w, ap);
        afree(t->vars, ap);
    }

    if (t->args != NULL) {
        /*XXX we assume the caller is right */
        union qsh_ccphack cw;

        cw.ro = t->args;
        for (w = cw.rw; *w != NULL; w++)
            afree(*w, ap);
        afree(t->args, ap);
    }

    if (t->ioact != NULL)
        iofree(t->ioact, ap);

    tfree(t->left, ap);
    tfree(t->right, ap);

    afree(t, ap);
}

static void
iofree(struct ioword **iow, Area *ap)
{
    struct ioword **iop;
    struct ioword *p;

    iop = iow;
    while ((p = *iop++) != NULL) {
        afree(p->ioname, ap);
        afree(p->delim, ap);
        afree(p->heredoc, ap);
        afree(p, ap);
    }
    afree(iow, ap);
}

void
fpFUNCTf(struct shf *shf, int i, bool isksh, const char *k, struct op *v)
{
    if (isksh)
        fptreef(shf, i, "%s %s %T", Tfunction, k, v);
    else if (ktsearch(&keywords, k, hash(k)))
        fptreef(shf, i, "%s %s() %T", Tfunction, k, v);
    else
        fptreef(shf, i, "%s() %T", k, v);
}
