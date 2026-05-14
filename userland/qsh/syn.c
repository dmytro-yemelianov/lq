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

/* test-builtin helpers — still file-local */
extern Test_op dbtestp_isa(Test_env *, Test_meta);
extern const char *dbtestp_getopnd(Test_env *, Test_op, bool);
extern int dbtestp_eval(Test_env *, Test_op, const char *, const char *, bool);
extern void dbtestp_error(Test_env *, int, const char *) QSH_A_NORETURN;

struct op *outtree;           /* yyparse output */
struct nesting_state nesting; /* \n changed to ; */

bool reject; /* token(cf) gets symbol again */
int symbol;  /* yylex value */


const char Tcbrace[] = "}";
const char Tesac[] = "esac";

void
yyparse(bool doalias)
{
    int c;

    ACCEPT;

    outtree = c_list(doalias ? ALIAS : 0, source->type == SSTRING);
    c = tpeek(0);
    if (c == 0 && !outtree)
        outtree = newtp(TEOF);
    else if (!cinttype(c, C_LF | C_NUL))
        syntaxerr(NULL);
}

struct op *pipeline(int cf, int sALIAS)
{
    struct op *t, *p, *tl = NULL;

    t = get_command(cf, sALIAS);
    if (t != NULL) {
        while (token(0) == '|') {
            if ((p = get_command(CONTIN, sALIAS)) == NULL)
                syntaxerr(NULL);
            if (tl == NULL)
                t = tl = block(TPIPE, t, p);
            else
                tl = tl->right = block(TPIPE, tl->right, p);
        }
        REJECT;
    }
    return (t);
}

struct op *andor(int sALIAS)
{
    struct op *t, *p;
    int c;

    t = pipeline(0, sALIAS);
    if (t != NULL) {
        while ((c = token(0)) == LOGAND || c == LOGOR) {
            if ((p = pipeline(CONTIN, sALIAS)) == NULL)
                syntaxerr(NULL);
            t = block(c == LOGAND ? TAND : TOR, t, p);
        }
        REJECT;
    }
    return (t);
}

struct op *c_list(int sALIAS, bool multi)
{
    struct op *t = NULL, *p, *tl = NULL;
    int c;
    bool have_sep;

    while (/* CONSTCOND */ 1) {
        p = andor(sALIAS);
        /*
         * Token has always been read/rejected at this point, so
         * we don't worry about what flags to pass token()
         */
        c = token(0);
        have_sep = true;
        if (c == '\n' && (multi || inalias(source))) {
            if (!p)
                /* ignore blank lines */
                continue;
        } else if (!p)
            break;
        else if (c == '&' || c == COPROC)
            p = block(c == '&' ? TASYNC : TCOPROC, p, NULL);
        else if (c != ';')
            have_sep = false;
        if (!t)
            t = p;
        else if (!tl)
            t = tl = block(TLIST, t, p);
        else
            tl = tl->right = block(TLIST, tl->right, p);
        if (!have_sep)
            break;
    }
    REJECT;
    return (t);
}

static const char IONDELIM_delim[] = {CHAR, '<', CHAR, '<', EOS};

struct ioword *synio(int cf)
{
    struct ioword *iop;
    bool ishere;

    if (tpeek(cf) != REDIR)
        return (NULL);
    ACCEPT;
    iop = yylval.iop;
    if (iop->ioflag & IOSYNIONEXT) {
        iop->ioflag &= ~IOSYNIONEXT;
        return (iop);
    }
    ishere = (iop->ioflag & IOTYPE) == IOHERE;
    if (iop->ioflag & IOHERESTR) {
        musthave(LWORD, 0);
    } else if (ishere && tpeek(HEREDELIM) == '\n') {
        ACCEPT;
        yylval.cp = wdcopy(IONDELIM_delim, ATEMP);
        iop->ioflag |= IOEVAL | IONDELIM;
    } else
        musthave(LWORD, ishere ? HEREDELIM : 0);
    if (ishere) {
        iop->delim = yylval.cp;
        if (*ident != 0 && !(iop->ioflag & IOHERESTR)) {
            /* unquoted */
            iop->ioflag |= IOEVAL;
        }
        if (herep > &heres[HERES - 1])
            yyerror(Tf_toomany, "<<");
        *herep++ = iop;
    } else
        iop->ioname = yylval.cp;

    if (iop->ioflag & IOBASH) {
        iop->ioflag &= ~IOBASH;

        yylval.iop = alloc(sizeof(struct ioword), ATEMP);
        yylval.iop->ioname = alloc(3U, ATEMP);
        yylval.iop->ioname[0] = CHAR;
        yylval.iop->ioname[1] = digits_lc[iop->unit];
        yylval.iop->ioname[2] = EOS;
        yylval.iop->delim = NULL;
        yylval.iop->heredoc = NULL;
        yylval.iop->ioflag = IODUP | IOSYNIONEXT;
        yylval.iop->unit = 2;
        REJECT;
        symbol = REDIR;
    }
    return (iop);
}

struct op *nested(int type, unsigned int smark, unsigned int emark, int sALIAS)
{
    struct op *t;
    struct nesting_state old_nesting;

    nesting_push(&old_nesting, (int)smark);
    t = c_list(sALIAS, true);
    musthave(emark, KEYWORD | sALIAS);
    nesting_pop(&old_nesting);
    return (block(type, t, NULL));
}

static const char builtin_cmd[] = {QCHAR, '\\', CHAR, 'b',  CHAR, 'u',  CHAR, 'i', CHAR,
                                   'l',   CHAR, 't',  CHAR, 'i',  CHAR, 'n',  EOS};
static const char let_cmd[] = {CHAR, 'l', CHAR, 'e', CHAR, 't', EOS};
static const char setA_cmd0[] = {CHAR, 's', CHAR, 'e', CHAR, 't', EOS};
static const char setA_cmd1[] = {CHAR, '-', CHAR, 'A', EOS};
static const char setA_cmd2[] = {CHAR, '-', CHAR, '-', EOS};

struct op *get_command(int cf, int sALIAS)
{
    struct op *t;
    int c, iopn = 0, syniocf, lno;
    struct ioword *iop;
    XPtrV args, vars;
    struct nesting_state old_nesting;
    bool check_decl_utility;
    struct ioword *iops[NUFILE + 1];

    XPinit(args, 16);
    XPinit(vars, 16);

    syniocf = KEYWORD | sALIAS;
    switch (c = token(cf | KEYWORD | sALIAS | CMDASN)) {
    default:
        REJECT;
        XPfree(args);
        XPfree(vars);
        /* empty line */
        return (NULL);

    case LWORD:
    case REDIR:
        REJECT;
        syniocf &= ~(KEYWORD | sALIAS);
        t = newtp(TCOM);
        t->lineno = source->line;
        goto get_command_start;

    get_command_loop:
        if (XPsize(args) == 0) {
        get_command_start:
            check_decl_utility = true;
            cf = sALIAS | CMDASN;
        } else if (t->u.evalflags)
            cf = CMDWORD | CMDASN;
        else
            cf = CMDWORD;

        switch (tpeek(cf)) {
        case REDIR:
            while ((iop = synio(cf)) != NULL) {
                if (iopn >= NUFILE)
                    yyerror(Tf_toomany, Tredirection);
                iops[iopn++] = iop;
            }
            goto get_command_loop;

        case LWORD:
            ACCEPT;
            if (check_decl_utility) {
                struct tbl *tt = get_builtin(ident);
                kui flag;

                flag = tt ? tt->flag : 0;
                if (flag & DECL_UTIL)
                    t->u.evalflags = DOVACHECK;
                if (!(flag & DECL_FWDR))
                    check_decl_utility = false;
            }
            if ((XPsize(args) == 0 || Flag(FKEYWORD)) && is_wdvarassign(yylval.cp, false))
                XPput(vars, yylval.cp);
            else
                XPput(args, yylval.cp);
            goto get_command_loop;

        case ORD('(' /*)*/):
            if (XPsize(args) == 0 && XPsize(vars) == 1 && is_wdvarassign(yylval.cp, true)) {
                char *tcp;

                /* wdarrassign: foo=(bar) */
                ACCEPT;

                /* manipulate the vars string */
                tcp = XPptrv(vars)[(vars.len = 0)];
                /* 'varname=' -> 'varname' */
                tcp[wdscan(tcp, EOS) - tcp - 3] = EOS;

                /* construct new args strings */
                XPput(args, wdcopy(builtin_cmd, ATEMP));
                XPput(args, wdcopy(setA_cmd0, ATEMP));
                XPput(args, wdcopy(setA_cmd1, ATEMP));
                XPput(args, tcp);
                XPput(args, wdcopy(setA_cmd2, ATEMP));

                /* slurp in words till closing paren */
                while (token(CONTIN) == LWORD)
                    XPput(args, yylval.cp);
                if (symbol != /*(*/ ')')
                    syntaxerr(NULL);
                break;
            }

            afree(t, ATEMP);

            /*
             * Check for "> foo (echo hi)" which AT&T ksh allows
             * (not POSIX, but not disallowed)
             */
            if (XPsize(args) == 0 && XPsize(vars) == 0) {
                ACCEPT;
                goto Subshell;
            }

            /* must be a function */
            if (iopn != 0 || XPsize(args) != 1 || XPsize(vars) != 0)
                syntaxerr(NULL);
            ACCEPT;
            musthave(/*(*/ ')', 0);
            t = function_body(XPptrv(args)[0], sALIAS, false);
            break;
        }
        break;

    case ORD('(' /*)*/): {
        unsigned int subshell_nesting_type_saved;
    Subshell:
        subshell_nesting_type_saved = subshell_nesting_type;
        subshell_nesting_type = ORD(')');
        t = nested(TPAREN, ORD('('), ORD(')'), sALIAS);
        subshell_nesting_type = subshell_nesting_type_saved;
        break;
    }

    case ORD('{' /*}*/):
        t = nested(TBRACE, ORD('{'), ORD('}'), sALIAS);
        break;

    case MDPAREN:
        /* leave KEYWORD in syniocf (allow if (( 1 )) then ...) */
        lno = source->line;
        ACCEPT;
        switch (token(LETEXPR)) {
        case LWORD:
            break;
        case ORD('(' /*)*/):
            c = ORD('(');
            goto Subshell;
        default:
            syntaxerr(NULL);
        }
        t = newtp(TCOM);
        t->lineno = lno;
        XPput(args, wdcopy(builtin_cmd, ATEMP));
        XPput(args, wdcopy(let_cmd, ATEMP));
        XPput(args, yylval.cp);
        break;

    case DBRACKET: /* [[ .. ]] */
        /* leave KEYWORD in syniocf (allow if [[ -n 1 ]] then ...) */
        t = newtp(TDBRACKET);
        ACCEPT;
        {
            Test_env te;

            te.flags = TEF_DBRACKET;
            te.pos.av = &args;
            te.isa = dbtestp_isa;
            te.getopnd = dbtestp_getopnd;
            te.eval = dbtestp_eval;
            te.error = dbtestp_error;

            test_parse(&te);
        }
        break;

    case FOR:
    case SELECT:
        t = newtp((c == FOR) ? TFOR : TSELECT);
        musthave(LWORD, CMDASN);
        if (!is_wdvarname(yylval.cp, true))
            yyerror("%s: bad identifier", c == FOR ? "for" : Tselect);
        strdupx(t->str, ident, ATEMP);
        nesting_push(&old_nesting, c);
        t->vars = wordlist(sALIAS);
        t->left = dogroup(sALIAS);
        nesting_pop(&old_nesting);
        break;

    case WHILE:
    case UNTIL:
        nesting_push(&old_nesting, c);
        t = newtp((c == WHILE) ? TWHILE : TUNTIL);
        t->left = c_list(sALIAS, true);
        if (!t->left)
            syntaxerr(NULL);
        t->right = dogroup(sALIAS);
        if (!t->right)
            syntaxerr(NULL);
        nesting_pop(&old_nesting);
        break;

    case CASE:
        t = newtp(TCASE);
        musthave(LWORD, 0);
        t->str = yylval.cp;
        nesting_push(&old_nesting, c);
        t->left = caselist(sALIAS);
        nesting_pop(&old_nesting);
        break;

    case IF:
        nesting_push(&old_nesting, c);
        t = newtp(TIF);
        t->left = c_list(sALIAS, true);
        t->right = thenpart(sALIAS);
        musthave(FI, KEYWORD | sALIAS);
        nesting_pop(&old_nesting);
        break;

    case BANG:
        syniocf &= ~(KEYWORD | sALIAS);
        t = pipeline(0, sALIAS);
        if (t == NULL)
            syntaxerr(NULL);
        t = block(TBANG, NULL, t);
        break;

    case TIME:
        syniocf &= ~(KEYWORD | sALIAS);
        t = pipeline(0, sALIAS);
        if (t && t->type == TCOM) {
            t->str = alloc(2, ATEMP);
            /* TF_* flags */
            t->str[0] = '\0';
            t->str[1] = '\0';
        }
        t = block(TTIME, t, NULL);
        break;

    case FUNCTION:
        musthave(LWORD, 0);
        t = function_body(yylval.cp, sALIAS, true);
        break;
    }

    while ((iop = synio(syniocf)) != NULL) {
        if (iopn >= NUFILE)
            yyerror(Tf_toomany, Tredirection);
        iops[iopn++] = iop;
    }

    if (iopn == 0) {
        t->ioact = NULL;
    } else {
        iops[iopn++] = NULL;
        t->ioact = alloc2(iopn, sizeof(struct ioword *), ATEMP);
        memcpy(t->ioact, iops, iopn * sizeof(struct ioword *));
    }

    if (t->type == TCOM || t->type == TDBRACKET) {
        XPput(args, NULL);
        t->args = (const char **)XPclose(args);
        XPput(vars, NULL);
        t->vars = (char **)XPclose(vars);
    } else {
        XPfree(args);
        XPfree(vars);
    }

    if (c == MDPAREN) {
        t = block(TBRACE, t, NULL);
        t->ioact = t->left->ioact;
        t->left->ioact = NULL;
    }

    return (t);
}

