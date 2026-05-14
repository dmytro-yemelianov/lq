/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * test(1) / [ / [[ expression evaluator.
 *
 *  oexpr   ::= aexpr | aexpr "-o" oexpr ;
 *  aexpr   ::= nexpr | nexpr "-a" aexpr ;
 *  nexpr   ::= primary | "!" nexpr ;
 *  primary ::= unary-operator operand
 *      | operand binary-operator operand
 *      | operand
 *      | "(" oexpr ")" ;
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/* POSIX says > 1 for errors */
#define T_ERR_EXIT 2

#define test_access(name, mode) access((name), (mode))
#define test_stat(name, buffer) stat((name), (buffer))
#define test_lstat(name, buffer) lstat((name), (buffer))

static int test_oexpr(Test_env *, bool);
static int test_aexpr(Test_env *, bool);
static int test_nexpr(Test_env *, bool);
static int test_primary(Test_env *, bool);
static Test_op ptest_isa(Test_env *, Test_meta);
static const char *ptest_getopnd(Test_env *, Test_op, bool);
static void ptest_error(Test_env *, int, const char *);
static int mtimecmp(const struct stat *, const struct stat *);

int
c_test(const char **wp)
{
    int argc, rv, invert = 0;
    Test_env te;
    Test_op op;
    Test_meta tm;
    const char *lhs, **swp;

    te.flags = 0;
    te.isa = ptest_isa;
    te.getopnd = ptest_getopnd;
    te.eval = test_eval;
    te.error = ptest_error;

    for (argc = 0; wp[argc]; argc++)
        ;

    if (strcmp(wp[0], Tbracket) == 0) {
        if (strcmp(wp[--argc], "]") != 0) {
            bi_errorf("missing ]");
            return (T_ERR_EXIT);
        }
    }

    te.pos.wp = wp + 1;
    te.wp_end = wp + argc;

    /*
     * Attempt to conform to POSIX special cases. This is pretty
     * dumb code straight-forward from the 2008 spec, but unlike
     * the old pdksh code doesn't live from so many assumptions.
     * It does, though, inline some calls to '(*te.funcname)()'.
     */
    switch (argc - 1) {
    case 0:
        return (1);
    case 1:
    ptest_one:
        op = TO_STNZE;
        goto ptest_unary;
    case 2:
    ptest_two:
        if (ptest_isa(&te, TM_NOT)) {
            ++invert;
            goto ptest_one;
        }
        if ((op = ptest_isa(&te, TM_UNOP))) {
        ptest_unary:
            rv = test_eval(&te, op, *te.pos.wp++, NULL, true);
        ptest_out:
            if (te.flags & TEF_ERROR)
                return (T_ERR_EXIT);
            return ((invert & 1) ? rv : !rv);
        }
        /* let the parser deal with anything else */
        break;
    case 3:
    ptest_three:
        swp = te.pos.wp;
        /* use inside knowledge of ptest_getopnd inlined below */
        lhs = *te.pos.wp++;
        if ((op = ptest_isa(&te, TM_BINOP))) {
            /* test lhs op rhs */
            rv = test_eval(&te, op, lhs, *te.pos.wp++, true);
            goto ptest_out;
        }
        if (ptest_isa(&te, tm = TM_AND) || ptest_isa(&te, tm = TM_OR)) {
            /* XSI */
            argc = test_eval(&te, TO_STNZE, lhs, NULL, true);
            rv = test_eval(&te, TO_STNZE, *te.pos.wp++, NULL, true);
            if (tm == TM_AND)
                rv = argc && rv;
            else
                rv = argc || rv;
            goto ptest_out;
        }
        /* back up to lhs */
        te.pos.wp = swp;
        if (ptest_isa(&te, TM_NOT)) {
            ++invert;
            goto ptest_two;
        }
        if (ptest_isa(&te, TM_OPAREN)) {
            swp = te.pos.wp;
            /* skip operand, without evaluation */
            te.pos.wp++;
            /* check for closing parenthesis */
            op = ptest_isa(&te, TM_CPAREN);
            /* back up to operand */
            te.pos.wp = swp;
            /* if there was a closing paren, handle it */
            if (op)
                goto ptest_one;
            /* backing up is done before calling the parser */
        }
        /* let the parser deal with it */
        break;
    case 4:
        if (ptest_isa(&te, TM_NOT)) {
            ++invert;
            goto ptest_three;
        }
        if (ptest_isa(&te, TM_OPAREN)) {
            swp = te.pos.wp;
            /* skip two operands, without evaluation */
            te.pos.wp++;
            te.pos.wp++;
            /* check for closing parenthesis */
            op = ptest_isa(&te, TM_CPAREN);
            /* back up to first operand */
            te.pos.wp = swp;
            /* if there was a closing paren, handle it */
            if (op)
                goto ptest_two;
            /* backing up is done before calling the parser */
        }
        /* defer this to the parser */
        break;
    }

    /* "The results are unspecified." */
    te.pos.wp = wp + 1;
    return (test_parse(&te));
}

/*
 * Generic test routines.
 */

Test_op
test_isop(Test_meta meta, const char *s)
{
    char sc1;
    const struct t_op *tbl;

    tbl = meta == TM_UNOP ? u_ops : b_ops;
    if (*s) {
        sc1 = s[1];
        for (; tbl->op_text[0]; tbl++)
            if (sc1 == tbl->op_text[1] && !strcmp(s, tbl->op_text))
                return (tbl->op_num);
    }
    return (TO_NONOP);
}

static int
mtimecmp(const struct stat *sb1, const struct stat *sb2)
{
    if (sb1->st_mtime < sb2->st_mtime)
        return (-1);
    if (sb1->st_mtime > sb2->st_mtime)
        return (1);
    return (0);
}

int
test_eval(Test_env *te, Test_op op, const char *opnd1, const char *opnd2, bool do_eval)
{
    int i, s;
    size_t k;
    struct stat b1, b2;
    qsh_ari_t v1, v2;
    struct tbl *vp;

    if (!do_eval)
        return (0);

    switch (op) {

    /*
     * Unary Operators
     */

    /* -n */
    case TO_STNZE:
        return (*opnd1 != '\0');

    /* -z */
    case TO_STZER:
        return (*opnd1 == '\0');

    /* -v */
    case TO_ISSET:
        return ((vp = isglobal(opnd1, false)) && (vp->flag & ISSET));

    /* -o */
    case TO_OPTION:
        if ((i = *opnd1) == '!' || i == '?')
            opnd1++;
        if ((k = option(opnd1)) == (size_t)-1)
            return (0);
        return (i == '?' ? 1 : i == '!' ? !Flag(k) : Flag(k));

    /* -r */
    case TO_FILRD:
        /* LINTED use of access */
        return (test_access(opnd1, R_OK) == 0);

    /* -w */
    case TO_FILWR:
        /* LINTED use of access */
        return (test_access(opnd1, W_OK) == 0);

    /* -x */
    case TO_FILEX:
        return (qsh_access(opnd1, X_OK) == 0);

    /* -a */
    case TO_FILAXST:
    /* -e */
    case TO_FILEXST:
        return (test_stat(opnd1, &b1) == 0);

    /* -f */
    case TO_FILREG:
        return (test_stat(opnd1, &b1) == 0 && S_ISREG(b1.st_mode));

    /* -d */
    case TO_FILID:
        return (test_stat(opnd1, &b1) == 0 && S_ISDIR(b1.st_mode));

    /* -c */
    case TO_FILCDEV:
        return (test_stat(opnd1, &b1) == 0 && S_ISCHR(b1.st_mode));

    /* -b */
    case TO_FILBDEV:
        return (test_stat(opnd1, &b1) == 0 && S_ISBLK(b1.st_mode));

    /* -p */
    case TO_FILFIFO:
        return (test_stat(opnd1, &b1) == 0 && S_ISFIFO(b1.st_mode));

    /* -h or -L */
    case TO_FILSYM:
        return (test_lstat(opnd1, &b1) == 0 && S_ISLNK(b1.st_mode));

    /* -S */
    case TO_FILSOCK:
        return (test_stat(opnd1, &b1) == 0 && S_ISSOCK(b1.st_mode));

    /* -H => HP CDF — not on POSIX systems */
    case TO_FILCDF:
        return (0);

    /* -u */
    case TO_FILSETU:
        return (test_stat(opnd1, &b1) == 0 && (b1.st_mode & S_ISUID) == S_ISUID);

    /* -g */
    case TO_FILSETG:
        return (test_stat(opnd1, &b1) == 0 && (b1.st_mode & S_ISGID) == S_ISGID);

    /* -k */
    case TO_FILSTCK:
        return (test_stat(opnd1, &b1) == 0 && (b1.st_mode & S_ISVTX) == S_ISVTX);

    /* -s */
    case TO_FILGZ:
        return (test_stat(opnd1, &b1) == 0 && (off_t)b1.st_size > (off_t)0);

    /* -t */
    case TO_FILTT:
        if (opnd1 && !bi_getn(opnd1, &i)) {
            te->flags |= TEF_ERROR;
            i = 0;
        } else
            i = isatty(opnd1 ? i : 0);
        return (i);

    /* -O */
    case TO_FILUID:
        return (test_stat(opnd1, &b1) == 0 && (uid_t)b1.st_uid == qsheuid);

    /* -G */
    case TO_FILGID:
        return (test_stat(opnd1, &b1) == 0 && (gid_t)b1.st_gid == qshegid);

    /*
     * Binary Operators
     */

    /* =, == */
    case TO_STEQL:
        if (te->flags & TEF_DBRACKET) {
            if ((i = gmatchx(opnd1, opnd2, false)))
                record_match(opnd1);
            return (i);
        }
        return (strcmp(opnd1, opnd2) == 0);

    /* != */
    case TO_STNEQ:
        if (te->flags & TEF_DBRACKET) {
            if ((i = gmatchx(opnd1, opnd2, false)))
                record_match(opnd1);
            return (!i);
        }
        return (strcmp(opnd1, opnd2) != 0);

    /* < */
    case TO_STLT:
        return (strcmp(opnd1, opnd2) < 0);

    /* > */
    case TO_STGT:
        return (strcmp(opnd1, opnd2) > 0);

    /* -nt */
    case TO_FILNT:
        /*
         * ksh88/ksh93 succeed if file2 can't be stated
         * (subtly different from 'does not exist').
         */
        return (test_stat(opnd1, &b1) == 0 &&
                (((s = test_stat(opnd2, &b2)) == 0 && mtimecmp(&b1, &b2) > 0) || s < 0));

    /* -ot */
    case TO_FILOT:
        /*
         * ksh88/ksh93 succeed if file1 can't be stated
         * (subtly different from 'does not exist').
         */
        return (test_stat(opnd2, &b2) == 0 &&
                (((s = test_stat(opnd1, &b1)) == 0 && mtimecmp(&b1, &b2) < 0) || s < 0));

    /* -ef */
    case TO_FILEQ:
        return (test_stat(opnd1, &b1) == 0 && test_stat(opnd2, &b2) == 0 &&
                b1.st_dev == b2.st_dev && b1.st_ino == b2.st_ino);

    /* all other cases */
    case TO_NONOP:
    case TO_NONNULL:
        /* throw the error */
        break;

    /* -eq */
    case TO_INTEQ:
    /* -ne */
    case TO_INTNE:
    /* -ge */
    case TO_INTGE:
    /* -gt */
    case TO_INTGT:
    /* -le */
    case TO_INTLE:
    /* -lt */
    case TO_INTLT:
        if (!evaluate(opnd1, &v1, QSH_RETURN_ERROR, false) ||
            !evaluate(opnd2, &v2, QSH_RETURN_ERROR, false)) {
            /* error already printed.. */
            te->flags |= TEF_ERROR;
            return (1);
        }
        switch (op) {
        case TO_INTEQ:
            return (v1 == v2);
        case TO_INTNE:
            return (v1 != v2);
        case TO_INTGE:
            return (v1 >= v2);
        case TO_INTGT:
            return (v1 > v2);
        case TO_INTLE:
            return (v1 <= v2);
        case TO_INTLT:
            return (v1 < v2);
        default:
            /* NOTREACHED */
            break;
        }
        /* NOTREACHED */
    }
    (*te->error)(te, 0, "internal error: unknown op");
    return (1);
}

int
test_parse(Test_env *te)
{
    int rv;

    rv = test_oexpr(te, 1);

    if (!(te->flags & TEF_ERROR) && !(*te->isa)(te, TM_END))
        (*te->error)(te, 0, "unexpected operator/operand");

    return ((te->flags & TEF_ERROR) ? T_ERR_EXIT : !rv);
}

static int
test_oexpr(Test_env *te, bool do_eval)
{
    int rv;

    if ((rv = test_aexpr(te, do_eval)))
        do_eval = false;
    if (!(te->flags & TEF_ERROR) && (*te->isa)(te, TM_OR))
        return (test_oexpr(te, do_eval) || rv);
    return (rv);
}

static int
test_aexpr(Test_env *te, bool do_eval)
{
    int rv;

    if (!(rv = test_nexpr(te, do_eval)))
        do_eval = false;
    if (!(te->flags & TEF_ERROR) && (*te->isa)(te, TM_AND))
        return (test_aexpr(te, do_eval) && rv);
    return (rv);
}

static int
test_nexpr(Test_env *te, bool do_eval)
{
    if (!(te->flags & TEF_ERROR) && (*te->isa)(te, TM_NOT))
        return (!test_nexpr(te, do_eval));
    return (test_primary(te, do_eval));
}

static int
test_primary(Test_env *te, bool do_eval)
{
    const char *opnd1, *opnd2;
    int rv;
    Test_op op;

    if (te->flags & TEF_ERROR)
        return (0);
    if ((*te->isa)(te, TM_OPAREN)) {
        rv = test_oexpr(te, do_eval);
        if (te->flags & TEF_ERROR)
            return (0);
        if (!(*te->isa)(te, TM_CPAREN)) {
            (*te->error)(te, 0, "missing )");
            return (0);
        }
        return (rv);
    }
    /*
     * Binary should have precedence over unary in this case
     * so that something like test \( -f = -f \) is accepted
     */
    if ((te->flags & TEF_DBRACKET) ||
        (&te->pos.wp[1] < te->wp_end && !test_isop(TM_BINOP, te->pos.wp[1]))) {
        if ((op = (*te->isa)(te, TM_UNOP))) {
            /* unary expression */
            opnd1 = (*te->getopnd)(te, op, do_eval);
            if (!opnd1) {
                (*te->error)(te, -1, Tno_args);
                return (0);
            }

            return ((*te->eval)(te, op, opnd1, NULL, do_eval));
        }
    }
    opnd1 = (*te->getopnd)(te, TO_NONOP, do_eval);
    if (!opnd1) {
        (*te->error)(te, 0, "expression expected");
        return (0);
    }
    if ((op = (*te->isa)(te, TM_BINOP))) {
        /* binary expression */
        opnd2 = (*te->getopnd)(te, op, do_eval);
        if (!opnd2) {
            (*te->error)(te, -1, "missing second argument");
            return (0);
        }

        return ((*te->eval)(te, op, opnd1, opnd2, do_eval));
    }
    return ((*te->eval)(te, TO_STNZE, opnd1, NULL, do_eval));
}

/*
 * Plain test (test and [ .. ]) specific routines.
 */

/*
 * Test if the current token is a whatever. Accepts the current token if
 * it is. Returns 0 if it is not, non-zero if it is (in the case of
 * TM_UNOP and TM_BINOP, the returned value is a Test_op).
 */
static Test_op
ptest_isa(Test_env *te, Test_meta meta)
{
    /* Order important - indexed by Test_meta values */
    static const char *const tokens[] = {Tdo, Tda, "!", "(", ")"};
    Test_op rv;

    if (te->pos.wp >= te->wp_end)
        return (meta == TM_END ? TO_NONNULL : TO_NONOP);

    if (meta == TM_UNOP || meta == TM_BINOP)
        rv = test_isop(meta, *te->pos.wp);
    else if (meta == TM_END)
        rv = TO_NONOP;
    else
        rv = !strcmp(*te->pos.wp, tokens[(int)meta]) ? TO_NONNULL : TO_NONOP;

    /* Accept the token? */
    if (rv != TO_NONOP)
        te->pos.wp++;

    return (rv);
}

static const char *
ptest_getopnd(Test_env *te, Test_op op, bool do_eval QSH_A_UNUSED)
{
    if (te->pos.wp >= te->wp_end)
        return (op == TO_FILTT ? "1" : NULL);
    return (*te->pos.wp++);
}

static void
ptest_error(Test_env *te, int ofs, const char *msg)
{
    const char *op;

    te->flags |= TEF_ERROR;
    if ((op = te->pos.wp + ofs >= te->wp_end ? NULL : te->pos.wp[ofs]))
        bi_errorf(Tf_sD_s, op, msg);
    else
        bi_errorf(Tf_s, msg);
}
