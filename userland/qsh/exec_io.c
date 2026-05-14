/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/* Forward decls for siblings in exec.c */
extern int search_access(const char *fn, int mode);
extern const char *search_path(const char *name, const char *lpath, int mode, int *errnop);
extern struct tbl *findcom(const char *name, int flags);

int call_builtin(struct tbl *tp, const char **wp, const char *where, bool resetspec);
static int hereinval(struct ioword *iop, int sub, char **resbuf, struct shf *shf);
/* iosetup, herein are declared in proto.h (publicly used) */

int
call_builtin(struct tbl *tp, const char **wp, const char *where, bool resetspec)
{
    int rv;
    bool old_builtin_spec;

    if (!tp)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_TWOMSG | KWF_NOERRNO, where, wp[0]);
    builtin_argv0 = wp[0];
    old_builtin_spec = builtin_spec;
    builtin_spec = ((bool)(!resetspec && (tp->flag & SPEC_BI)));
    shf_reopen(1, SHF_WR, shl_stdout);
    shl_stdout_ok = true;
    qsh_getopt_reset(&builtin_opt, GF_ERROR);
    rv = (*tp->val.f)(wp);
    if (shf_flush(shl_stdout) < 0) {
        bi_errorf(Tf_sD_s, Twrite, cstrerror(errno));
        if (rv == 0)
            rv = 1;
    }
    shl_stdout_ok = false;
    builtin_argv0 = NULL;
    builtin_spec = old_builtin_spec;
    return (rv);
}

/*
 * set up redirection, saving old fds in e->savedfd
 */
int
iosetup(struct ioword *iop, struct tbl *tp)
{
    int u = -1;
    char *cp = iop->ioname;
    int iotype = iop->ioflag & IOTYPE;
    bool do_open = true, do_close = false, do_fstat = false;
    int flags = 0;
    struct ioword iotmp;
    struct stat statb;

    if (iotype != IOHERE)
        cp = evalonestr(cp, DOTILDE | (Flag(FTALKING_I) ? DOGLOB : 0));

    /* Used for tracing and error messages to print expanded cp */
    iotmp = *iop;
    iotmp.ioname = (iotype == IOHERE) ? NULL : cp;
    iotmp.ioflag |= IONAMEXP;

    if (Flag(FXTRACE)) {
        change_xtrace(2, false);
        fptreef(shl_xtrace, 0, Tft_R, &iotmp);
        change_xtrace(1, false);
    }

    switch (iotype) {
    case IOREAD:
        flags = O_RDONLY;
        break;

    case IOCAT:
        flags = O_WRONLY | O_APPEND | O_CREAT;
        break;

    case IOWRITE:
        if (Flag(FNOCLOBBER) && !(iop->ioflag & IOCLOB)) {
            /* >file under set -C */
            if (stat(cp, &statb)) {
                /* nonexistent file */
                flags = O_WRONLY | O_CREAT | O_EXCL;
            } else if (S_ISREG(statb.st_mode)) {
                /* regular file, refuse clobbering */
                goto clobber_refused;
            } else {
                /*
                 * allow redirections to things
                 * like /dev/null without error
                 */
                flags = O_WRONLY;
                /* but check again after opening */
                do_fstat = true;
            }
        } else {
            /* >|file or set +C */
            flags = O_WRONLY | O_CREAT | O_TRUNC;
        }
        break;

    case IORDWR:
        flags = O_RDWR | O_CREAT;
        break;

    case IOHERE:
        do_open = false;
        /* herein() returns -2 if error has been printed */
        u = herein(iop, NULL);
        /* cp may have wrong name */
        break;

    case IODUP: {
        const char *emsg;

        do_open = false;
        if (qsh_isdash(cp)) {
            /* prevent error return below */
            u = 1009;
            do_close = true;
        } else if ((u = check_fd(cp, X_OK | ((iop->ioflag & IORDUP) ? R_OK : W_OK), &emsg)) < 0) {
            char *sp;

            sp = snptreef(NULL, 32, Tft_R, &iotmp);
            kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, sp, emsg);
            afree(sp, ATEMP);
            return (-1);
        }
        if (u == (int)iop->unit) {
            /* "dup from" == "dup to" */
            iop->ioflag |= IODUPSELF;
            return (0);
        }
        break;
    }
    }

    if (do_open) {
        if (Flag(FRESTRICTED) && (flags & O_CREAT)) {
            kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, cp, "restricted");
            return (-1);
        }
        u = binopen3(cp, flags, 0666);
        if (do_fstat && u >= 0) {
            /* prevent race conditions */
            if (fstat(u, &statb) || S_ISREG(statb.st_mode)) {
                close(u);
            clobber_refused:
                u = -1;
                errno = EEXIST;
            }
        }
    }
    if (u < 0) {
        /* herein() may already have printed message */
        if (u == -1) {
            kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG, cp,
                   (iotype == IOREAD || iotype == IOHERE) ? Topen :
                                                          Tcreate);
        }
        return (-1);
    }
    /* only save if it has not yet been redirected (e.g. by "cat >x >y") */
    if (FDSVNUM(e, iop->unit) == 0U) {
        /* c_exec() assumes e->savedfd[fd] set for any redirection */
        FDSAVE(iop->unit, u == (int)iop->unit ?
                                              /* previously closed (exec >&-; ls >x; print e) */ -1
                                              : savefd(iop->unit));
    } else
        /* clear previous fd-was-closed flag */
        e->savedfd[iop->unit] &= FDNUMMASK;

    if (do_close) {
        close(iop->unit);
        e->savedfd[iop->unit] |= FDICLMASK;
    } else if (u != (int)iop->unit) {
        if (qsh_dup2(u, iop->unit, true) < 0) {
            int eno;
            char *sp;

            eno = errno;
            sp = snptreef(NULL, 32, Tft_s_R, Tredirection_dup, &iotmp);
            kwarnf(KWF_VERRNO | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG, eno, sp);
            afree(sp, ATEMP);
            if (iotype != IODUP)
                close(u);
            return (-1);
        }
        if (iotype != IODUP)
            close(u);
        /*
         * Touching any co-process fd in an empty exec
         * causes the shell to close its copies
         */
        else if (tp && tp->type == CSHELL && tp->val.f == c_exec) {
            if (iop->ioflag & IORDUP)
                /* possible exec <&p */
                coproc_read_close(u);
            else
                /* possible exec >&p */
                coproc_write_close(u);
        }
    }
    if (u == 2)
        /* Clear any write errors */
        shf_reopen(2, SHF_WR, shl_out);
    return (0);
}

/*
 * Process here documents by providing the content, either as
 * result (globally allocated) string or in a temp file; if
 * unquoted, the string is expanded first.
 */
static int
hereinval(struct ioword *iop, int sub, char **resbuf, struct shf *shf)
{
    const char *volatile ccp = iop->heredoc;
    struct source *s, *osource;

    osource = source;
    newenv(E_ERRH);
    if (qshsetjmp(e->jbuf)) {
        source = osource;
        quitenv(shf);
        /* special to iosetup(): don't print error */
        return (-2);
    }
    if (iop->ioflag & IOHERESTR) {
        ccp = evalstr(iop->delim, DOHERESTR | DOSCALAR);
    } else if (sub) {
        /* do substitutions on the content of heredoc */
        s = pushs(SSTRING, ATEMP);
        s->start = s->str = ccp;
        source = s;
        if (yylex(sub) != LWORD)
            kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_ONEMSG | KWF_NOERRNO, "herein: yylex");
        source = osource;
        ccp = evalstr(yylval.cp, DOSCALAR | DOHEREDOC);
    }

    if (resbuf == NULL)
        shf_puts(ccp, shf);
    else
        strdupx(*resbuf, ccp, APERM);

    quitenv(NULL);
    return (0);
}

int
herein(struct ioword *iop, char **resbuf)
{
    int fd = -1;
    struct shf *shf;
    struct temp *h;
    int i;

    /* lexer substitution flags */
    i = (iop->ioflag & IOEVAL) ? (ONEWORD | HEREDOC) : 0;

    /* skip all the fd setup if we just want the value */
    if (resbuf != NULL)
        return (hereinval(iop, i, resbuf, NULL));

    /*
     * Create temp file to hold content (done before newenv
     * so temp doesn't get removed too soon).
     */
    h = maketemp(ATEMP, TT_HEREDOC_EXP, &e->temps);
    if (!(shf = h->shf) || (fd = binopen3(h->tffn, O_RDONLY, 0)) < 0) {
        kwarnf0(KWF_PREFIX | KWF_FILELINE, Tf_temp, !shf ? Tcreate : Topen, h->tffn);
        if (shf)
            shf_close(shf);
        /* special to iosetup(): don't print error */
        return (-2);
    }

    if (hereinval(iop, i, NULL, shf) == -2) {
        close(fd);
        /* special to iosetup(): don't print error */
        return (-2);
    }

    if (shf_close(shf) == -1) {
        i = errno;
        close(fd);
        kwarnf1(KWF_VERRNO | KWF_PREFIX | KWF_FILELINE, i, Tf_temp, Twrite, h->tffn);
        /* special to iosetup(): don't print error */
        return (-2);
    }

    return (fd);
}

/*
 *  ksh special - the select command processing section
 *  print the args in column form - assuming that we can
 */
const char *
do_selectargs(const char **ap, bool print_menu)
{
    static const char *read_args[] = {Tread, Tdr, TREPLY, NULL};
    char *s;
    int i, argct;

    for (argct = 0; ap[argct]; argct++)
        ;
    while (/* CONSTCOND */ 1) {
        /*-
         * Menu is printed if
         *  - this is the first time around the select loop
         *  - the user enters a blank line
         *  - the REPLY parameter is empty
         */
        if (print_menu || !*str_val(global(TREPLY)))
            pr_menu(ap);
        shellf(Tf_s, str_val(global("PS3")));
        if (call_builtin(findcom(Tread, FC_BI), read_args, Tselect, false))
            return (NULL);
        if (*(s = str_val(global(TREPLY))))
            return ((getn(s, &i) && i >= 1 && i <= argct) ? ap[i - 1] : null);
        print_menu = true;
    }
}

struct select_menu_info {
    const char *const *args;
    int num_width;
};

/* format a single select menu item */
static void
select_fmt_entry(char *buf, size_t buflen, unsigned int i, const void *arg)
{
    const struct select_menu_info *smi = (const struct select_menu_info *)arg;

    shf_snprintf(buf, buflen, "%*u) %s", smi->num_width, i + 1, smi->args[i]);
}

/*
 *  print a select style menu
 */
void
pr_menu(const char *const *ap)
{
    struct select_menu_info smi;
    const char *const *pp;
    size_t acols = 0, aocts = 0, i;
    unsigned int n;
    struct columnise_opts co;

    /*
     * width/column calculations were done once and saved, but this
     * means select can't be used recursively so we re-calculate
     * each time (could save in a structure that is returned, but
     * it's probably not worth the bother)
     */

    /*
     * get dimensions of the list
     */
    for (n = 0, pp = ap; *pp; n++, pp++) {
        i = strlen(*pp);
        if (i > aocts)
            aocts = i;
        i = utf_mbswidth(*pp);
        if (i > acols)
            acols = i;
    }

    /*
     * we will print an index of the form "%d) " in front of
     * each entry, so get the maximum width of this
     */
    for (i = n, smi.num_width = 1; i >= 10; i /= 10)
        smi.num_width++;

    smi.args = ap;
    co.shf = shl_out;
    co.linesep = '\n';
    co.prefcol = co.do_last = true;
    print_columns(&co, n, select_fmt_entry, (void *)&smi, smi.num_width + 2 + aocts,
                  smi.num_width + 2 + acols);
}

static void
plain_fmt_entry(char *buf, size_t buflen, unsigned int i, const void *arg)
{
    strlcpy(buf, ((const char *const *)arg)[i], buflen);
}

void
pr_list(struct columnise_opts *cop, char *const *ap)
{
    size_t acols = 0, aocts = 0, i;
    unsigned int n;
    char *const *pp;

    for (n = 0, pp = ap; *pp; n++, pp++) {
        i = strlen(*pp);
        if (i > aocts)
            aocts = i;
        i = utf_mbswidth(*pp);
        if (i > acols)
            acols = i;
    }

    print_columns(cop, n, plain_fmt_entry, (const void *)ap, aocts, acols);
}

/*
 *  [[ ... ]] evaluation routines
 */

/*
 * Test if the current token is a whatever. Accepts the current token if
 * it is. Returns 0 if it is not, non-zero if it is (in the case of
 * TM_UNOP and TM_BINOP, the returned value is a Test_op).
 */
Test_op
dbteste_isa(Test_env *te, Test_meta meta)
{
    Test_op ret = TO_NONOP;
    bool uqword;
    const char *p;

    if (!*te->pos.wp)
        return (meta == TM_END ? TO_NONNULL : TO_NONOP);

    /* unquoted word? */
    for (p = *te->pos.wp; *p == CHAR; p += 2)
        ;
    uqword = *p == EOS;

    if (meta == TM_UNOP || meta == TM_BINOP) {
        if (uqword) {
            /* longer than the longest operator */
            char buf[8];
            char *q = buf;

            p = *te->pos.wp;
            while (*p++ == CHAR && (size_t)(q - buf) < sizeof(buf) - 1)
                *q++ = *p++;
            *q = '\0';
            ret = test_isop(meta, buf);
        }
    } else if (meta == TM_END)
        ret = TO_NONOP;
    else
        ret = (uqword && !strcmp(*te->pos.wp, dbtest_tokens[(int)meta])) ? TO_NONNULL : TO_NONOP;

    /* Accept the token? */
    if (ret != TO_NONOP)
        te->pos.wp++;

    return (ret);
}

const char *
dbteste_getopnd(Test_env *te, Test_op op, bool do_eval)
{
    const char *s = *te->pos.wp;
    int flags = DOTILDE | DOSCALAR;

    if (!s)
        return (NULL);

    te->pos.wp++;

    if (!do_eval)
        return (null);

    if (op == TO_STEQL || op == TO_STNEQ) {
        flags |= DOPAT;
        if (!Flag(FSH))
            flags |= DODBMAGIC;
    }

    return (evalstr(s, flags));
}

void
dbteste_error(Test_env *te, int offset, const char *msg)
{
    te->flags |= TEF_ERROR;
    kwarnf0(KWF_INTERNAL | KWF_WARNING | KWF_NOERRNO, "dbteste_error: %s (offset %d)", msg, offset);
}

const char **
cpyargv(int *i, const char **src, Area *ap)
{
    size_t n;
    const char **wp, **dst;

    wp = src - 1;
    while (*++wp != NULL)
        /* nothing */;
    n = wp - src;
    if (n < 1 || notoktoadd(n, 1) || notok2add((size_t)INT_MAX, n, 1))
        kerrf(KWF_VERRNO | KWF_INTERNAL | KWF_ERR(0xFF) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG,
              EOVERFLOW, "cpyargv");
    if (i)
        *i = n - /* qshname */ 1U;
    dst = alloc2(n + /* NULL */ 1U, sizeof(const char *), ap);

    wp = dst;
    *wp++ = *src++;
    while (--n)
        strdupx(*wp++, *src++, ap);
    *wp = NULL;
    return (dst);
}
