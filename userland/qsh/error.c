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


void
yyerror(const char *fmt, ...)
{
    va_list ap;

    /* pop aliases and re-reads */
    while (source->type == SALIAS || source->type == SREREAD)
        source = source->next;
    /* zap pending input */
    source->str = null;

    va_start(ap, fmt);
    vwarnf0(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_NOERRNO, 0, fmt, ap);
    va_end(ap);
    unwind(LSHELL);
}

/* used by error reporting functions to print "qsh: .qshrc[25]: " */
bool
error_prefix(bool fileline)
{
    bool qshname_shown = false;

    /* Avoid foo: foo[2]: ... */
    if (!fileline || !source || !source->file || strcmp(source->file, qshname) != 0) {
        qshname_shown = true;
        shf_puts(qshname + (isch(*qshname, '-') ? 1 : 0), shl_out);
        shf_putc_i(':', shl_out);
        shf_putc_i(' ', shl_out);
    }
    if (fileline && source && source->file != NULL) {
        shf_fprintf(shl_out, "%s[%d]: ", source->file,
                    source->errline ? source->errline : source->line);
        source->errline = 0;
    }
    return (qshname_shown);
}

/* error reporting functions */

void
vwarnf(unsigned int flags, int verrno, const char *fmt, va_list ap)
{
    int rept = 0;
    bool show_builtin_argv0 = false;

    if (HAS(flags, KWF_ERROR)) {
        if (HAS(flags, KWF_INTERNAL))
            shf_write(SC("internal error: "), shl_out);
        else
            shf_write(SC("E: "), shl_out);
        /* additional things to do on error */
        exstat = flags & KWF_EXSTAT;
        if (HAS(flags, KWF_INTERNAL) && trap_exstat != -1)
            trap_exstat = exstat;
        /* debugging: note that stdout not valid */
        shl_stdout_ok = false;
    } else {
        if (HAS(flags, KWF_INTERNAL))
            shf_write(SC("internal warning: "), shl_out);
        else
            shf_write(SC("W: "), shl_out);
    }
    if (HAS(flags, KWF_BUILTIN) &&
        /* not set when main() calls parse_args() */
        builtin_argv0 && builtin_argv0 != qshname)
        show_builtin_argv0 = true;
    if (HAS(flags, KWF_PREFIX) && error_prefix(HAS(flags, KWF_FILELINE)) && show_builtin_argv0) {
        const char *qshbasename;

        qshname_islogin(&qshbasename);
        show_builtin_argv0 = strcmp(builtin_argv0, qshbasename) != 0;
    }
    if (show_builtin_argv0) {
        shf_puts(builtin_argv0, shl_out);
        shf_putc_i(':', shl_out);
        shf_putc_i(' ', shl_out);
    }
    switch (flags & KWF_MSGMASK) {
    default:
#undef shf_vfprintf
        shf_vfprintf(shl_out, fmt, ap);
#define shf_vfprintf poisoned_shf_vfprintf
        break;
    case KWF_THREEMSG:
        rept = 2;
        if (0)
            /* FALLTHROUGH */
        case KWF_TWOMSG:
            rept = 1;
        /* FALLTHROUGH */
    case KWF_ONEMSG:
        while (/* CONSTCOND */ 1) {
            /* The ternary already guarantees a non-NULL pointer
             * (Tnil is an extern array), so the shf_puts macro's
             * NULL test is moot — sidestep -Waddress with a direct
             * shf_write. */
            const char *p = fmt ? fmt : Tnil;
            shf_write(p, strlen(p), shl_out);
            if (!rept--)
                break;
            shf_putc_i(':', shl_out);
            shf_putc_i(' ', shl_out);
            fmt = va_arg(ap, const char *);
        }
        break;
    }
    if (!HAS(flags, KWF_NOERRNO)) {
        /* compare shf.c */
        /* may be nil */
        shf_fprintf(shl_out, ": %s", cstrerror(verrno));
    }
    shf_putc_i('\n', shl_out);
    shf_flush(shl_out);
}

void
vwarnf0(unsigned int flags, int verrno, const char *fmt, va_list ap)
{
    vwarnf(flags, verrno, fmt, ap);
}

void
kwarnf(unsigned int flags, ...)
{
    const char *fmt;
    va_list ap;
    int verrno;

    verrno = errno;

    va_start(ap, flags);
    if (HAS(flags, KWF_VERRNO))
        verrno = va_arg(ap, int);
    fmt = va_arg(ap, const char *);
    vwarnf(flags, verrno, fmt, ap);
    va_end(ap);
    if (HAS(flags, KWF_BIUNWIND))
        bi_unwind(0);
}

void
kwarnf0(unsigned int flags, const char *fmt, ...)
{
    va_list ap;
    int verrno;

    verrno = errno;

    va_start(ap, fmt);
    vwarnf0(flags, verrno, fmt, ap);
    va_end(ap);
    if (HAS(flags, KWF_BIUNWIND))
        bi_unwind(0);
}

void
kwarnf1(unsigned int flags, int verrno, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vwarnf0(flags, verrno, fmt, ap);
    va_end(ap);
    if (HAS(flags, KWF_BIUNWIND))
        bi_unwind(0);
}

/* presented by lack of portable variadic macros in early C */

void
kerrf(unsigned int flags, ...)
{
    const char *fmt;
    va_list ap;
    int verrno;

    verrno = errno;

    va_start(ap, flags);
    if (HAS(flags, KWF_VERRNO))
        verrno = va_arg(ap, int);
    fmt = va_arg(ap, const char *);
    vwarnf(flags, verrno, fmt, ap);
    va_end(ap);
    unwind(LERROR);
}

void
kerrf0(unsigned int flags, const char *fmt, ...)
{
    va_list ap;
    int verrno;

    verrno = errno;

    va_start(ap, fmt);
    vwarnf0(flags, verrno, fmt, ap);
    va_end(ap);
    unwind(LERROR);
}

/* maybe error, maybe builtin error; use merrf() macro */
void
merrF(int *ep, unsigned int flags, ...)
{
    const char *fmt;
    va_list ap;
    int verrno;

    verrno = errno;

    if (ep)
        flags |= KWF_BUILTIN;

    va_start(ap, flags);
    if (HAS(flags, KWF_VERRNO))
        verrno = va_arg(ap, int);
    fmt = va_arg(ap, const char *);
    vwarnf(flags, verrno, fmt, ap);
    va_end(ap);

    if (ep) {
        *ep = exstat;
        bi_unwind(0);
    } else
        unwind(LERROR);
}

/* transform warning into bi_errorf */
void
bi_unwind(int rc)
{
    if (rc)
        exstat = rc;
    /* debugging: note that stdout not valid */
    shl_stdout_ok = false;

    /* POSIX special builtins cause non-interactive shells to exit */
    if (builtin_spec) {
        builtin_argv0 = NULL;
        /* may not want to use LERROR here */
        unwind(LERROR);
    }
}

/*XXX old */
void
bi_errorf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vwarnf0(/*XXX*/ KWF_BIERR | KWF_NOERRNO, /*XXX*/ 0, fmt, ap);
    va_end(ap);
    bi_unwind(0);
}
