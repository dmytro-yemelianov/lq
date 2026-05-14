/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define QSH_SHF_VFPRINTF_NO_GCC_FORMAT_ATTRIBUTE
#include "sh.h"
#include "syn_priv.h"


struct op *
dogroup(int sALIAS)
{
    int c;
    struct op *list;

    c = token(CONTIN | KEYWORD | sALIAS);
    /*
     * A {...} can be used instead of do...done for for/select loops
     * but not for while/until loops - we don't need to check if it
     * is a while loop because it would have been parsed as part of
     * the conditional command list...
     */
    if (c == DO)
        c = DONE;
    else if ((unsigned int)c == ORD('{'))
        c = ORD('}');
    else
        syntaxerr(NULL);
    list = c_list(sALIAS, true);
    musthave(c, KEYWORD | sALIAS);
    return (list);
}

struct op *
thenpart(int sALIAS)
{
    struct op *t;

    musthave(THEN, KEYWORD | sALIAS);
    t = newtp(0);
    t->left = c_list(sALIAS, true);
    if (t->left == NULL)
        syntaxerr(NULL);
    t->right = elsepart(sALIAS);
    return (t);
}

struct op *
elsepart(int sALIAS)
{
    struct op *t;

    switch (token(KEYWORD | sALIAS | CMDASN)) {
    case ELSE:
        if ((t = c_list(sALIAS, true)) == NULL)
            syntaxerr(NULL);
        return (t);

    case ELIF:
        t = newtp(TELIF);
        t->left = c_list(sALIAS, true);
        t->right = thenpart(sALIAS);
        return (t);

    default:
        REJECT;
    }
    return (NULL);
}

struct op *
caselist(int sALIAS)
{
    struct op *t, *tl;
    int c;

    c = token(CONTIN | KEYWORD | sALIAS);
    /* A {...} can be used instead of in...esac for case statements */
    if (c == IN)
        c = ESAC;
    else if ((unsigned int)c == ORD('{'))
        c = ORD('}');
    else
        syntaxerr(NULL);
    t = tl = NULL;
    /* no ALIAS here */
    while ((tpeek(CONTIN | KEYWORD | ESACONLY)) != c) {
        struct op *tc = casepart(c, sALIAS);
        if (tl == NULL)
            t = tl = tc, tl->right = NULL;
        else
            tl->right = tc, tl = tc;
    }
    musthave(c, KEYWORD | sALIAS);
    return (t);
}

struct op *
casepart(int endtok, int sALIAS)
{
    struct op *t;
    XPtrV ptns;

    XPinit(ptns, 16);
    t = newtp(TPAT);
    /* no ALIAS here */
    if ((unsigned int)token(CONTIN | KEYWORD) != ORD('('))
        REJECT;
    do {
        switch (token(0)) {
        case LWORD:
            break;
        case ORD('}'):
        case ESAC:
            if (symbol != endtok) {
                strdupx(yylval.cp, (unsigned int)symbol == ORD('}') ? Tcbrace : Tesac, ATEMP);
                break;
            }
            /* FALLTHROUGH */
        default:
            syntaxerr(NULL);
        }
        XPput(ptns, yylval.cp);
    } while (token(0) == '|');
    REJECT;
    XPput(ptns, NULL);
    t->vars = (char **)XPclose(ptns);
    musthave(ORD(')'), 0);

    t->left = c_list(sALIAS, true);

    /* initialise to default for ;; or omitted */
    t->u.charflag = ORD(';');
    /* SUSv4 requires the ;; except in the last casepart */
    if ((tpeek(CONTIN | KEYWORD | sALIAS)) != endtok)
        switch (symbol) {
        default:
            syntaxerr(NULL);
        case BRKEV:
            t->u.charflag = ORD('|');
            if (0)
                /* FALLTHROUGH */
            case BRKFT:
                t->u.charflag = ORD('&');
            /* FALLTHROUGH */
        case BREAK:
            /* initialised above, but we need to eat the token */
            ACCEPT;
        }
    return (t);
}

struct op *
function_body(char *name, int sALIAS,
              /* function foo { ... } vs foo() { .. } */
              bool qsh_func)
{
    char *sname, *p;
    struct op *t;

    sname = wdstrip(name, 0);
    /*-
     * Check for valid characters in name. POSIX and AT&T ksh93 say
     * only allow [a-zA-Z_0-9] but this allows more as old pdkshs
     * have allowed more; the following were never allowed:
     *  NUL TAB NL SP " $ & ' ( ) ; < = > \ ` |
     * C_QUOTE|C_SPC covers all but adds # * ? [ ]
     * CiQCM, as desired, adds / but also ^ (as collateral)
     */
    for (p = sname; *p; p++)
        if (ctype(*p, C_QUOTE | C_SPC | CiQCM))
            yyerror(Tinvname, sname, Tfunction);

    /*
     * Note that POSIX allows only compound statements after foo(),
     * sh and AT&T ksh allow any command, go with the later since it
     * shouldn't break anything. However, for function foo, AT&T ksh
     * only accepts an open-brace.
     */
    if (qsh_func) {
        if ((unsigned int)tpeek(CONTIN | KEYWORD | sALIAS) == ORD('(' /*)*/)) {
            /* function foo () { //}*/
            ACCEPT;
            musthave(ORD(/*(*/ ')'), 0);
            /* degrade to POSIX function */
            qsh_func = false;
        }
        musthave(ORD('{' /*}*/), CONTIN | KEYWORD | sALIAS);
        REJECT;
    }

    t = newtp(TFUNCT);
    t->str = sname;
    t->u.qsh_func = ((bool)(qsh_func));
    t->lineno = source->line;

    if ((t->left = get_command(CONTIN, sALIAS)) == NULL) {
        char *tv;
        /*
         * Probably something like foo() followed by EOF or ';'.
         * This is accepted by sh and ksh88.
         * To make "typeset -f foo" work reliably (so its output can
         * be used as input), we pretend there is a colon here.
         */
        t->left = newtp(TCOM);
        /* (2 * sizeof(char *)) is small enough */
        t->left->args = alloc(2 * sizeof(char *), ATEMP);
        t->left->args[0] = tv = alloc(3, ATEMP);
        tv[0] = QCHAR;
        tv[1] = ':';
        tv[2] = EOS;
        t->left->args[1] = NULL;
        t->left->vars = alloc(sizeof(char *), ATEMP);
        t->left->vars[0] = NULL;
        t->left->lineno = 1;
    }

    return (t);
}

char **
wordlist(int sALIAS)
{
    int c;
    XPtrV args;

    XPinit(args, 16);
    /* POSIX does not do alias expansion here... */
    if ((c = token(CONTIN | KEYWORD | sALIAS)) != IN) {
        if (c != ';')
            /* non-POSIX, but AT&T ksh accepts a ; here */
            REJECT;
        return (NULL);
    }
    while ((c = token(0)) == LWORD)
        XPput(args, yylval.cp);
    if (c != '\n' && c != ';')
        syntaxerr(NULL);
    XPput(args, NULL);
    return ((char **)XPclose(args));
}

/*
 * supporting functions
 */

struct op *
block(int type, struct op *t1, struct op *t2)
{
    struct op *t;

    t = newtp(type);
    t->left = t1;
    t->right = t2;
    return (t);
}

static const struct tokeninfo {
    const char *name;
    short val;
    short reserved;
} tokentab[] = {
    /* Reserved words */
    {"if", IF, true},
    {"then", THEN, true},
    {"else", ELSE, true},
    {"elif", ELIF, true},
    {"fi", FI, true},
    {"case", CASE, true},
    {Tesac, ESAC, true},
    {"for", FOR, true},
    {Tselect, SELECT, true},
    {"while", WHILE, true},
    {"until", UNTIL, true},
    {"do", DO, true},
    {"done", DONE, true},
    {"in", IN, true},
    {Tfunction, FUNCTION, true},
    {Ttime, TIME, true},
    {"{", ORD('{'), true},
    {Tcbrace, ORD('}'), true},
    {"!", BANG, true},
    {"[[", DBRACKET, true},
    /* Lexical tokens (0[EOF], LWORD and REDIR handled specially) */
    {"&&", LOGAND, false},
    {"||", LOGOR, false},
    {";;", BREAK, false},
    {";|", BRKEV, false},
    {";&", BRKFT, false},
    {"((", MDPAREN, false},
    {"|&", COPROC, false},
    /* and some special cases... */
    {"newline", ORD('\n'), false},
    {NULL, 0, false}};

void
initkeywords(void)
{
    struct tokeninfo const *tt;
    struct tbl *p;

    ktinit(APERM, &keywords,
           /* currently 28 keywords: 75% of 64 = 2^6 */
           6);
    for (tt = tokentab; tt->name; tt++) {
        if (tt->reserved) {
            p = ktenter(&keywords, tt->name, hash(tt->name));
            p->flag |= DEFINED | ISSET;
            p->type = CKEYWD;
            p->val.i = tt->val;
        }
    }
}

void
syntaxerr(const char *what)
{
    /* 23<<- is the longest redirection, I think */
    char redir[8];
    const char *s;
    struct tokeninfo const *tt;
    int c;

    if (!what)
        what = Tunexpected;
    REJECT;
    c = token(0);
Again:
    switch (c) {
    case 0:
        if (nesting.start_token) {
            c = nesting.start_token;
            source->errline = nesting.start_line;
            what = "unmatched";
            goto Again;
        }
        /* don't quote the EOF */
        yyerror("%s: unexpected EOF", Tsynerr);
        /* NOTREACHED */

    case LWORD:
        s = snptreef(NULL, 32, Tf_S, yylval.cp);
        break;

    case REDIR:
        s = snptreef(redir, sizeof(redir), Tft_R, yylval.iop);
        break;

    default:
        for (tt = tokentab; tt->name; tt++)
            if (tt->val == c)
                break;
        if (tt->name)
            s = tt->name;
        else {
            if (c > 0 && c < 256) {
                redir[0] = c;
                redir[1] = '\0';
            } else
                shf_snprintf(redir, sizeof(redir), "?%d", c);
            s = redir;
        }
    }
    yyerror(Tf_sD_s_qs, Tsynerr, what, s);
}

void
nesting_push(struct nesting_state *save, int tok)
{
    *save = nesting;
    nesting.start_token = tok;
    nesting.start_line = source->line;
}

void
nesting_pop(struct nesting_state *saved)
{
    nesting = *saved;
}

struct op *
newtp(int type)
{
    struct op *t;

    t = alloc(sizeof(struct op), ATEMP);
    t->type = type;
    t->u.evalflags = 0;
    t->args = NULL;
    t->vars = NULL;
    t->ioact = NULL;
    t->left = t->right = NULL;
    t->str = NULL;
    return (t);
}

struct op *
compile(Source *s, bool doalias)
{
    nesting.start_token = 0;
    nesting.start_line = 0;
    herep = heres;
    source = s;
    yyparse(doalias);
    return (outtree);
}

/* Check if we are in the middle of reading an alias */
int
inalias(struct source *s)
{
    while (s && s->type == SALIAS) {
        if (!(s->flags & SF_ALIASEND))
            return (1);
        s = s->next;
    }
    return (0);
}

/*
 * Order important - indexed by Test_meta values
 * Note that ||, &&, ( and ) can't appear in as unquoted strings
 * in normal shell input, so these can be interpreted unambiguously
 * in the evaluation pass.
 */
static const char dbtest_or[] = {CHAR, '|', CHAR, '|', EOS};
static const char dbtest_and[] = {CHAR, '&', CHAR, '&', EOS};
static const char dbtest_not[] = {CHAR, '!', EOS};
static const char dbtest_oparen[] = {CHAR, '(', EOS};
static const char dbtest_cparen[] = {CHAR, ')', EOS};
const char *const dbtest_tokens[] = {dbtest_or, dbtest_and, dbtest_not, dbtest_oparen,
                                     dbtest_cparen};
static const char db_close[] = {CHAR, ']', CHAR, ']', EOS};
static const char db_lthan[] = {CHAR, '<', EOS};
static const char db_gthan[] = {CHAR, '>', EOS};

/*
 * Test if the current token is a whatever. Accepts the current token if
 * it is. Returns 0 if it is not, non-zero if it is (in the case of
 * TM_UNOP and TM_BINOP, the returned value is a Test_op).
 */
Test_op
dbtestp_isa(Test_env *te, Test_meta meta)
{
    int c = tpeek(CMDASN | (meta == TM_BINOP ? 0 : CONTIN));
    bool uqword;
    char *save = NULL;
    Test_op ret = TO_NONOP;

    /* unquoted word? */
    uqword = c == LWORD && *ident;

    if (meta == TM_OR)
        ret = c == LOGOR ? TO_NONNULL : TO_NONOP;
    else if (meta == TM_AND)
        ret = c == LOGAND ? TO_NONNULL : TO_NONOP;
    else if (meta == TM_NOT)
        ret = (uqword && !strcmp(yylval.cp, dbtest_tokens[(int)TM_NOT])) ? TO_NONNULL : TO_NONOP;
    else if (meta == TM_OPAREN)
        ret = (unsigned int)c == ORD('(') /*)*/ ? TO_NONNULL : TO_NONOP;
    else if (meta == TM_CPAREN)
        ret = (unsigned int)c == /*(*/ ORD(')') ? TO_NONNULL : TO_NONOP;
    else if (meta == TM_UNOP || meta == TM_BINOP) {
        if (meta == TM_BINOP && c == REDIR &&
            (yylval.iop->ioflag == IOREAD || yylval.iop->ioflag == IOWRITE)) {
            ret = TO_NONNULL;
            save = wdcopy(yylval.iop->ioflag == IOREAD ? db_lthan : db_gthan, ATEMP);
        } else if (uqword && (ret = test_isop(meta, ident)))
            save = yylval.cp;
    } else
        /* meta == TM_END */
        ret = (uqword && !strcmp(yylval.cp, db_close)) ? TO_NONNULL : TO_NONOP;
    if (ret != TO_NONOP) {
        ACCEPT;
        if ((unsigned int)meta < NELEM(dbtest_tokens))
            save = wdcopy(dbtest_tokens[(int)meta], ATEMP);
        if (save)
            XPput(*te->pos.av, save);
    }
    return (ret);
}

const char *
dbtestp_getopnd(Test_env *te, Test_op op QSH_A_UNUSED, bool do_eval QSH_A_UNUSED)
{
    int c = tpeek(CMDASN);

    if (c != LWORD)
        return (NULL);

    ACCEPT;
    XPput(*te->pos.av, yylval.cp);

    return (null);
}

int
dbtestp_eval(Test_env *te QSH_A_UNUSED, Test_op op QSH_A_UNUSED, const char *opnd1 QSH_A_UNUSED,
             const char *opnd2 QSH_A_UNUSED, bool do_eval QSH_A_UNUSED)
{
    return (1);
}

void
dbtestp_error(Test_env *te, int offset, const char *msg)
{
    te->flags |= TEF_ERROR;

    if (offset < 0) {
        REJECT;
        /* Kludgy to say the least... */
        symbol = LWORD;
        yylval.cp = *(XPptrv(*te->pos.av) + XPsize(*te->pos.av) + offset);
    }
    syntaxerr(msg);
}

bool
parse_usec(const char *s, struct timeval *tv)
{
    int i;

    tv->tv_sec = 0;
    /* parse integral part */
#define qiCfail                                                                                   \
    do {                                                                                           \
        errno = EOVERFLOW;                                                                         \
        return (true);                                                                               \
    } while (/* CONSTCOND */ 0)
    if (qiTYPE_ISF(time_t)) {
        time_t tt = 0;

        while (ctype(*s, C_DIGIT))
            tt = tt * 10 + qsh_numdig(*s++);
        qiCAAlet(tv->tv_sec, time_t, tt);
    } else if (qiTYPE_ISU(time_t)) {
        time_t tt = 0;

        while (ctype(*s, C_DIGIT)) {
            qiCAUmul(time_t, tt, 10);
            qiCAUadd(tt, qsh_numdig(*s));
            ++s;
        }
        qiCAAlet(tv->tv_sec, time_t, tt);
    } else {
        qiHUGE_S tt = 0;

        while (ctype(*s, C_DIGIT)) {
            qiCAPmul(qiHUGE_S, tt, 10);
            qiCAPadd(qiHUGE_S, tt, qsh_numdig(*s));
            ++s;
        }
        qiCASlet(time_t, tv->tv_sec, qiHUGE_S, tt);
    }
#undef qiCfail

    tv->tv_usec = 0;
    if (!*s)
        /* no decimal fraction */
        return (false);
    else if (*s++ != '.') {
        /* junk after integral part */
        errno = EINVAL;
        return (true);
    }

    /* parse decimal fraction */
    i = 100000;
    while (ctype(*s, C_DIGIT)) {
        tv->tv_usec += i * qsh_numdig(*s++);
        if (i == 1)
            break;
        i /= 10;
    }
    /* check for junk after fractional part */
    while (ctype(*s, C_DIGIT))
        ++s;
    if (*s) {
        errno = EINVAL;
        return (true);
    }

    /* end of input string reached, no errors */
    return (false);
}

/*
 * Helper function called from within lex.c:yylex() to parse
 * a COMSUB recursively using the main shell parser and lexer
 */
char *
yyrecursive(int subtype)
{
    struct op *t;
    char *cp;
    struct yyrecursive_state *ys;
    unsigned int stok, etok;

    if (subtype != COMSUB) {
        stok = ORD('{');
        etok = ORD('}');
    } else {
        stok = ORD('(');
        etok = ORD(')');
    }

    ys = alloc(sizeof(struct yyrecursive_state), ATEMP);

    /* tell the lexer to accept a closing parenthesis as EOD */
    ys->old_nesting_type = subshell_nesting_type;
    subshell_nesting_type = etok;

    /* push reject state, parse recursively, pop reject state */
    ys->old_reject = reject;
    ys->old_symbol = symbol;
    ACCEPT;
    memcpy(ys->old_heres, heres, sizeof(heres));
    ys->old_herep = herep;
    herep = heres;
    ys->next = e->yyrecursive_statep;
    e->yyrecursive_statep = ys;
    /* we use TPAREN as a helper container here */
    t = nested(TPAREN, stok, etok, ALIAS);
    yyrecursive_pop(false);

    /* t->left because nested(TPAREN, ...) hides our goodies there */
    cp = snptreef(NULL, 0, Tf_T, t->left);
    tfree(t, ATEMP);

    return (cp);
}

void
yyrecursive_pop(bool popall)
{
    struct yyrecursive_state *ys;

popnext:
    if (!(ys = e->yyrecursive_statep))
        return;
    e->yyrecursive_statep = ys->next;

    memcpy(heres, ys->old_heres, sizeof(heres));
    herep = ys->old_herep;
    reject = ys->old_reject;
    symbol = ys->old_symbol;

    subshell_nesting_type = ys->old_nesting_type;

    afree(ys, ATEMP);
    if (popall)
        goto popnext;
}

