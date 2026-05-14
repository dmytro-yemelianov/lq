/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/* Forward decls for helpers defined later in this file. */
static unsigned int dollarqU(struct shf *, const unsigned char *);
static void dollarq8(struct shf *, const unsigned char *);

int
ascstrcmp(const void *s1, const void *s2)
{
    const kby *cp1 = s1, *cp2 = s2;

    while (*cp1 == *cp2) {
        if (*cp1++ == '\0')
            return (0);
        ++cp2;
    }
    return ((int)asciibetical(*cp1) - (int)asciibetical(*cp2));
}

int
ascpstrcmp(const void *pstr1, const void *pstr2)
{
    return (ascstrcmp(*(const char *const *)pstr1, *(const char *const *)pstr2));
}

/* Initialise a Getopt structure */
void
qsh_getopt_reset(Getopt *go, int flags)
{
    memset(go, '\0', sizeof(Getopt));
    go->optind = 1;
    go->optarg = NULL;
    go->flags = flags;
}

/**
 * getopt() used for shell built-in commands, the getopts command, and
 * command line options.
 * A leading ':' in options means don't print errors, instead return '?'
 * or ':' and set go->optarg to the offending option character.
 * If GF_ERROR is set (and option doesn't start with :), errors result in
 * a call to bi_errorf().
 *
 * Non-standard features:
 *  - ';' is like ':' in options, except the argument is optional
 *    (if it isn't present, optarg is set to NULL).
 *    Used for 'set -o'.
 *  - ',' is like ':' in options, except the argument always immediately
 *    follows the option character (optarg is set to the null string if
 *    the option is missing).
 *    Used for 'read -u2', 'print -u2' and fc -40.
 *  - '#' is like ':' in options, expect that the argument is optional
 *    and must start with a digit. If the argument doesn't start with a
 *    digit, it is assumed to be missing and normal option processing
 *    continues (optarg is set to 0 if the option is missing).
 *    Used for 'typeset -LZ4'.
 *  - accepts +c as well as -c IF the GF_PLUSOPT flag is present. If an
 *    option starting with + is accepted, the GI_PLUS flag will be set
 *    in go->info.
 */
int
qsh_getopt(const char **argv, Getopt *go, const char *optionsp)
{
    char c;
    const char *o;

    if (go->p == 0 || (c = argv[go->optind - 1][go->p]) == '\0') {
        const char *arg = argv[go->optind], flag = arg ? *arg : '\0';

        go->p = 1;
        if (flag == '-' && qsh_isdash(arg + 1)) {
            go->optind++;
            go->p = 0;
            go->info |= GI_MINUSMINUS;
            return (-1);
        }
        if (arg == NULL ||
            ((flag != '-') &&
             /* neither a - nor a + (if + allowed) */
             (!(go->flags & GF_PLUSOPT) || flag != '+')) ||
            (c = arg[1]) == '\0') {
            go->p = 0;
            return (-1);
        }
        go->optind++;
        go->info &= ~(GI_MINUS | GI_PLUS);
        go->info |= flag == '-' ? GI_MINUS : GI_PLUS;
    }
    go->p++;
    if (ctype(c, C_QUEST | C_COLON | C_HASH) || c == ';' || c == ',' ||
        !(o = cstrchr(optionsp, c))) {
        if (optionsp[0] == ':') {
            go->buf[0] = c;
            go->optarg = go->buf;
        } else {
            qsh_getopt_opterr(c, (go->flags & GF_NONAME) ? null : argv[0], Tunknown_option);
            if (go->flags & GF_ERROR)
                bi_unwind(1);
        }
        return (ORD('?'));
    }
    /**
     * : means argument must be present, may be part of option argument
     *   or the next argument
     * ; same as : but argument may be missing
     * , means argument is part of option argument, and may be null.
     */
    if (*++o == ':' || *o == ';') {
        if (argv[go->optind - 1][go->p])
            go->optarg = argv[go->optind - 1] + go->p;
        else if (argv[go->optind])
            go->optarg = argv[go->optind++];
        else if (*o == ';')
            go->optarg = NULL;
        else {
            if (optionsp[0] == ':') {
                go->buf[0] = c;
                go->optarg = go->buf;
                return (ORD(':'));
            }
            qsh_getopt_opterr(c, (go->flags & GF_NONAME) ? null : argv[0], Treq_arg);
            if (go->flags & GF_ERROR)
                bi_unwind(1);
            return (ORD('?'));
        }
        go->p = 0;
    } else if (*o == ',') {
        /* argument is attached to option character, even if null */
        go->optarg = argv[go->optind - 1] + go->p;
        go->p = 0;
    } else if (*o == '#') {
        /*
         * argument is optional and may be attached or unattached
         * but must start with a digit. optarg is set to 0 if the
         * argument is missing.
         */
        if (argv[go->optind - 1][go->p]) {
            if (ctype(argv[go->optind - 1][go->p], C_DIGIT)) {
                go->optarg = argv[go->optind - 1] + go->p;
                go->p = 0;
            } else
                go->optarg = NULL;
        } else {
            if (argv[go->optind] && ctype(argv[go->optind][0], C_DIGIT)) {
                go->optarg = argv[go->optind++];
                go->p = 0;
            } else
                go->optarg = NULL;
        }
    }
    return (ord(c));
}

void
qsh_getopt_opterr(int ch, const char *name, const char *msg)
{
    static char buf[3] = {'-', QSH_BEL, '\0'};

    buf[1] = ch;
    if (name == null || name == qshname)
        kwarnf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, buf, msg);
    else
        kwarnf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_THREEMSG | KWF_NOERRNO, name, buf, msg);
}

/*
 * print variable/alias value using necessary quotes
 * (POSIX says they should be suitable for re-entry...)
 * No trailing newline is printed.
 */
void
print_value_quoted(struct shf *shf, const char *s)
{
    unsigned char c;
    const unsigned char *p = (const unsigned char *)s;
    bool inquote = true;

    /* first, special-case empty strings (for re-entrancy) */
    if (!*s) {
        shf_putc('\'', shf);
        shf_putc('\'', shf);
        return;
    }

    /* non-empty; check whether any quotes are needed */
    if (UTFMODE) {
        /* C1 always escaped; multibyte makes this tricky */
        while ((c = *p++) != 0) {
            if (ctype(c, C_CNTRL)) {
                dollarqU(shf, (const unsigned char *)s);
                return;
            }
            /* anything out of ASCII present? */
            if (rtt2asc(c) > 0x7EU) {
                /* dollar-quote, but be prepared to redo */
                char *guess;
                struct shf to;

                shf_sopen(NULL, 0, SHF_WR | SHF_DYNAMIC, &to);
                c = dollarqU(&to, (const unsigned char *)s);
                guess = shf_sclose(&to);
                /* output guess if it was right */
                if (c > 1)
                    shf_puts(guess, shf);
                afree(guess, ATEMP);
                if (c == 1)
                    goto always_single;
                if (c == 0)
                noquoteneeded:
                    shf_puts(s, shf);
                return;
            }
            if (ctype(c, C_QUOTE | C_SPC))
                inquote = false;
        }
        /* assert: c == 0; all chars in [20;7E] ASCII */
    } else if (Flag(FASIS)) {
        while ((c = *p++), !qsh_asisctrl(c))
            if (ctype(c, C_QUOTE | C_SPC))
                inquote = false;
    } else {
        while ((c = *p++), !qsh_isctrl(c))
            if (ctype(c, C_QUOTE | C_SPC))
                inquote = false;
    }
    /* state: if c == 0, all chars printable, inquote shortcuts */

    if (c) {
        /* otherwise, escape control chars */
        dollarq8(shf, (const unsigned char *)s);
        return;
    }

    /* can we shortcut? */
    if (inquote)
        goto noquoteneeded;
    /* no */
always_single:
    /* all chars printable, no control chars, quote nicely */
    inquote = false;
    p = (const unsigned char *)s;

    while ((c = *p++) != 0) {
        if (c == '\'') {
            if (inquote) {
                shf_scheck(3, shf);
                shf_putc('\'', shf);
                inquote = false;
            }
            shf_putc('\\', shf);
        } else if (!inquote) {
            shf_putc('\'', shf);
            inquote = true;
        }
        shf_putc(c, shf);
    }
    if (inquote)
        shf_putc('\'', shf);
}

#define dollarq_Uctrl(c) (!ctype((c), C_PRINT))
#define dollarq_isctrlU(c) dollarq_Uctrl(c)

/* escape with $'...' (!QSH_SMALL: in UTFMODE) */
static unsigned int
dollarqU(struct shf *shf, const unsigned char *s)
{
    unsigned char c;
    unsigned int wc;
    size_t n;
    unsigned int rv = 0;

    shf_putc('$', shf);
    shf_putc('\'', shf);
    while ((c = *s) != 0) {
        if (
            rtt2asc(c) >= 0xC2U && (n = utf_mbtowc(&wc, (const char *)s)) != (size_t)-1) {
            /* valid UTF-8 multibyte character > 0x7F */
            if ((wc ^ 0x80U) < 0x20U) {
                /* C1 control character */
                shf_fprintf(shf, "\\u%04X", wc);
                rv = 2;
                s += n;
            } else {
                /*
                 * print as-is; we assume the tty DTRT for
                 * interlinear annotations, LTR/RTL mark,
                 * U+2028, U+2029, U+2066..U+206F, etc.
                 */
                shf_wr_sm(s, n, shf);
            }
            continue;
        }
        ++s;
        /* single octet */
        rv |= ctype(c, C_QUOTE | C_SPC);
        switch (c) {
        /* see unbksl() in this file for comments */
        case QSH_BEL:
            c = 'a';
            if (0)
                /* FALLTHROUGH */
            case '\b':
                c = 'b';
            if (0)
                /* FALLTHROUGH */
            case '\f':
                c = 'f';
            if (0)
                /* FALLTHROUGH */
            case '\n':
                c = 'n';
            if (0)
                /* FALLTHROUGH */
            case '\r':
                c = 'r';
            if (0)
                /* FALLTHROUGH */
            case '\t':
                c = 't';
            if (0)
                /* FALLTHROUGH */
            case QSH_VTAB:
                c = 'v';
            if (0)
                /* FALLTHROUGH */
            case QSH_ESC:
                /* take E not e because \e is \ in *roff */
                c = 'E';
            rv = 2;
            /* FALLTHROUGH */
        case '\\':
            shf_putc('\\', shf);

            if (0)
                /* FALLTHROUGH */
            default:
                if (dollarq_isctrlU(c)) {
                    rv = 2;
                    /* FALLTHROUGH */
                case '\'':
                    shf_fprintf(shf, "\\%03o", c);
                    break;
                }

            shf_putc(c, shf);
            break;
        }
    }
    shf_putc('\'', shf);
    return (rv);
}

/* escape with $'...' outside UTFMODE */
static void
dollarq8(struct shf *shf, const unsigned char *s)
{
    unsigned char c;

    shf_putc('$', shf);
    shf_putc('\'', shf);
    while ((c = *s++) != 0) {
        /* single octet */
        switch (c) {
        /* see unbksl() in this file for comments */
        case QSH_BEL:
            c = 'a';
            if (0)
                /* FALLTHROUGH */
            case '\b':
                c = 'b';
            if (0)
                /* FALLTHROUGH */
            case '\f':
                c = 'f';
            if (0)
                /* FALLTHROUGH */
            case '\n':
                c = 'n';
            if (0)
                /* FALLTHROUGH */
            case '\r':
                c = 'r';
            if (0)
                /* FALLTHROUGH */
            case '\t':
                c = 't';
            if (0)
                /* FALLTHROUGH */
            case QSH_VTAB:
                c = 'v';
            if (0)
                /* FALLTHROUGH */
            case QSH_ESC:
                /* take E not e because \e is \ in *roff */
                c = 'E';
            /* FALLTHROUGH */
        case '\\':
            shf_putc('\\', shf);

            if (0)
                /* FALLTHROUGH */
            default:
                if (qsh_isctrl8(c)) {
                    /* FALLTHROUGH */
                case '\'':
                    shf_fprintf(shf, "\\%03o", c);
                    break;
                }

            shf_putc(c, shf);
            break;
        }
    }
    shf_putc('\'', shf);
}

/*
 * Print things in columns and rows - func() is called to format
 * the i-th element
 */
void
print_columns(struct columnise_opts *opts, unsigned int n,
              void (*func)(char *, size_t, unsigned int, const void *), const void *arg,
              size_t max_oct, size_t max_colz)
{
    unsigned int i, r = 0, c, rows, cols, nspace, max_col;
    char *str;

    if (!n)
        return;

    if (max_colz > 2147483646) {
        kwarnf0(KWF_INTERNAL | KWF_WARNING | KWF_NOERRNO,
                "print_columns called with %s=%zu >= INT_MAX", "max_col", max_colz);
        return;
    }
    max_col = (unsigned int)max_colz;

    if (max_oct > 2147483646) {
        kwarnf0(KWF_INTERNAL | KWF_WARNING | KWF_NOERRNO,
                "print_columns called with %s=%zu >= INT_MAX", "max_oct", max_oct);
        return;
    }
    ++max_oct;
    str = alloc(max_oct, ATEMP);

    /*
     * We use (max_col + 2) to consider the separator space.
     * Note that no spaces are printed after the last column
     * to avoid problems with terminals that have auto-wrap,
     * but we need to also take this into account in x_cols.
     */
    cols = (x_cols + 1) / (max_col + 2);

    /* if we can only print one column anyway, skip the goo */
    if (cols < 2) {
        goto prcols_easy;
        while (r < n) {
            shf_putc(opts->linesep, opts->shf);
        prcols_easy:
            (*func)(str, max_oct, r++, arg);
            shf_puts(str, opts->shf);
        }
        goto out;
    }

    rows = (n + cols - 1) / cols;
    if (opts->prefcol && cols > rows) {
        cols = rows;
        rows = (n + cols - 1) / cols;
    }

    nspace = (x_cols - max_col * cols) / cols;
    if (nspace < 2)
        nspace = 2;
    max_col = -max_col;
    goto prcols_hard;
    while (r < rows) {
        shf_putc(opts->linesep, opts->shf);
    prcols_hard:
        for (c = 0; c < cols; c++) {
            if ((i = c * rows + r) >= n)
                break;
            (*func)(str, max_oct, i, arg);
            if (i + rows >= n)
                shf_puts(str, opts->shf);
            else
                shf_fprintf(opts->shf, "%*s%*s", (int)max_col, str, (int)nspace, null);
        }
        ++r;
    }
out:
    if (opts->do_last)
        shf_putc(opts->linesep, opts->shf);
    afree(str, ATEMP);
}

/* strip all NUL bytes from buf; output is NUL-terminated if stripped */
void
strip_nuls(char *buf, size_t len)
{
    char *cp, *dp, *ep;

    rndpush(buf, len);
    if (!len || !(dp = memchr(buf, '\0', len)))
        return;

    ep = buf + len;
    cp = dp;

cp_has_nul_byte:
    while (cp++ < ep && *cp == '\0')
        ; /* nothing */
    while (cp < ep && *cp != '\0')
        *dp++ = *cp++;
    if (cp < ep)
        goto cp_has_nul_byte;

    *dp = '\0';
}

/*
 * Like read(2), but if read fails due to non-blocking flag,
 * resets flag and restarts read.
 */
ssize_t
blocking_read(int fd, char *buf, size_t nbytes)
{
    ssize_t ret;
    bool tried_reset = false;

    while ((ret = read(fd, buf, nbytes)) < 0) {
        if (!tried_reset && errno == EAGAIN) {
            if (reset_nonblock(fd) > 0) {
                tried_reset = true;
                continue;
            }
            errno = EAGAIN;
        }
        break;
    }
    return (ret);
}

/*
 * Reset the non-blocking flag on the specified file descriptor.
 * Returns -1 if there was an error, 0 if non-blocking wasn't set,
 * 1 if it was.
 */
int
reset_nonblock(int fd)
{
    int flags;

    if ((flags = fcntl(fd, F_GETFL, 0)) < 0)
        return (-1);
    if (!(flags & O_NONBLOCK))
        return (0);
    flags &= ~O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0)
        return (-1);
    return (1);
}

#ifndef ELOOP
#define ELOOP E2BIG
#endif

