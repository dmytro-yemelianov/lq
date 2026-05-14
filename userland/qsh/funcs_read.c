/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * The `read` builtin.  Big enough to deserve its own file.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

int
c_read(const char **wp)
{
#define is_ifsws(c) (ctype((c), C_IFS) && ctype((c), C_IFSWS))
    int c, fd = 0, rv = 0;
    bool savehist = false, intoarray = false, aschars = false;
    bool rawmode = false, expanding = false;
    bool lastparmmode = false, lastparmused = false;
    enum { LINES, BYTES, UPTO, READALL } readmode = LINES;
    kby delim = ORD('\n');
    size_t bytesleft = 128, bytesread;
    struct tbl *vp /* FU gcc */ = NULL, *vq = NULL;
    char *cp, *allocd = NULL, *xp;
    const char *ccp;
    XString xs;
    size_t xsave = 0;
    /* to catch read -aN2 foo[i] */
    bool subarray = false;
    k32 idx = 0;
    bool hastimeout = false;
    struct timeval tv, tvlim;

    while ((c = qsh_getopt(wp, &builtin_opt, "Aad:N:n:prst:u,")) != -1)
        switch (ord(c)) {
        case ORD('a'):
            aschars = true;
            /* FALLTHROUGH */
        case ORD('A'):
            intoarray = true;
            break;
        case ORD('d'):
            delim = ord(builtin_opt.optarg[0]);
            break;
        case ORD('N'):
            readmode = BYTES;
            if (0)
                /* FALLTHROUGH */
            case ORD('n'):
                readmode = UPTO;
            if (!bi_getn(builtin_opt.optarg, &c))
                return (2);
            if (c == -1) {
                readmode = readmode == BYTES ? READALL : UPTO;
                bytesleft = 1024;
            } else
                bytesleft = (unsigned int)c;
            break;
        case 'p':
            if ((fd = coproc_getfd(R_OK, &ccp)) < 0) {
                bi_errorf("%s: %s", Tdp, ccp);
                return (2);
            }
            break;
        case 'r':
            rawmode = true;
            break;
        case 's':
            savehist = true;
            break;
        case 't':
            if (parse_usec(builtin_opt.optarg, &tv)) {
                bi_errorf(Tf_sD_s_qs, Tsynerr, cstrerror(errno), builtin_opt.optarg);
                return (2);
            }
            hastimeout = true;
            break;
        case 'u':
            if (!builtin_opt.optarg[0])
                fd = 0;
            else if ((fd = check_fd(builtin_opt.optarg, R_OK, &ccp)) < 0) {
                kwarnf(KWF_ERR(2) | KWF_PREFIX | KWF_FILELINE | KWF_BUILTIN | KWF_BIUNWIND |
                           KWF_THREEMSG,
                       Tdu, builtin_opt.optarg, ccp);
                return (2);
            }
            break;
        case '?':
            return (2);
        }
    wp += builtin_opt.optind;
    if (*wp == NULL)
        *--wp = TREPLY;

    if (intoarray && wp[1] != NULL) {
        bi_errorf(Ttoo_many_args);
        return (2);
    }

    if ((ccp = cstrchr(*wp, '?')) != NULL) {
        strndupx(allocd, *wp, ccp - *wp, ATEMP);
        *wp = allocd;
        if (isatty(fd)) {
            /*
             * AT&T ksh says it prints prompt on fd if it's open
             * for writing and is a tty, but it doesn't do it
             * (it also doesn't check the interactive flag,
             * as is indicated in the Korn Shell book).
             */
            shf_puts(ccp + 1, shl_out);
            shf_flush(shl_out);
        }
    }

    Xinit(xs, xp, bytesleft, ATEMP);

    if (readmode == LINES)
        bytesleft = 1;
    /* read -N for byte-count reads is fine without termios: devc-ser8250
     * delivers raw bytes, no canonical-mode buffering to defeat. */

    if (hastimeout) {
        qsh_TIME(tvlim);
        timeradd(&tvlim, &tv, &tvlim);
    }

c_read_readloop:
    if (hastimeout) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int           ms;

        qsh_TIME(tv);
        if (timercmp(&tvlim, &tv, <)) {
            /* timeout expired globally */
            rv = 3;
            goto c_read_out;
        }
        timersub(&tvlim, &tv, &tv);
        /* convert remaining timeout to poll's milliseconds, capped at INT_MAX */
        if (tv.tv_sec >= (INT_MAX / 1000) - 1)
            ms = -1;
        else
            ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);

        switch (poll(&pfd, 1, ms)) {
        case 1:
            break;
        case 0:
            /* timeout expired for this call */
            bytesread = 0;
            rv = 3;
            goto c_read_readdone;
        default:
            bi_errorf(Tf_sD_s, "poll", cstrerror(errno));
            rv = 2;
            goto c_read_out;
        }
    }

    if ((bytesread = blocking_read(fd, xp, bytesleft)) == (size_t)-1) {
        if (errno == EINTR) {
            /* check whether the signal would normally kill */
            if (!fatal_trap_check()) {
                /* no, just ignore the signal */
                goto c_read_readloop;
            }
            /* pretend the read was killed */
        } else {
            /* unexpected error */
            bi_errorf(Tf_s, cstrerror(errno));
        }
        rv = 2;
        goto c_read_out;
    }

    switch (readmode) {
    case READALL:
        if (bytesread == 0) {
            /* end of file reached */
            rv = 1;
            goto c_read_readdone;
        }
        xp += bytesread;
        XcheckN(xs, xp, bytesleft);
        break;

    case UPTO:
        if (bytesread == 0)
            /* end of file reached */
            rv = 1;
        xp += bytesread;
        goto c_read_readdone;

    case BYTES:
        if (bytesread == 0) {
            /* end of file reached */
            rv = 1;
            /* may be partial read: $? = 1, but content */
            goto c_read_readdone;
        }
        xp += bytesread;
        if ((bytesleft -= bytesread) == 0)
            goto c_read_readdone;
        break;
    case LINES:
        if (bytesread == 0) {
            /* end of file reached */
            rv = 1;
            goto c_read_readdone;
        }
        if ((c = ord(*xp)) == '\0' && !aschars && delim != '\0') {
            /* skip any read NULs unless delimiter */
            break;
        }
        if (expanding) {
            expanding = false;
            if (ord(c) == ord(delim)) {
                if (Flag(FTALKING_I) && isatty(fd)) {
                    /*
                     * set prompt in case this is
                     * called from .profile or $ENV
                     */
                    set_prompt(PS2, NULL);
                    pprompt(prompt, 0);
                }
                /* drop the backslash */
                --xp;
                /* and the delimiter */
                break;
            }
        } else if (ord(c) == ord(delim)) {
            goto c_read_readdone;
        } else if (!rawmode && ord(c) == ORD('\\')) {
            expanding = true;
        }
        Xcheck(xs, xp);
        ++xp;
        break;
    }
    goto c_read_readloop;

c_read_readdone:
    bytesread = Xlength(xs, xp);
    Xput(xs, xp, '\0');

    /*-
     * state: we finished reading the input and NUL terminated it
     * Xstring(xs, xp) -> xp-1 = input string without trailing delim
     * rv = 3 if timeout, 1 if EOF, 0 otherwise (errors handled already)
     */

    if (rv) {
        /* clean up coprocess if needed, on EOF/error/timeout */
        coproc_read_close(fd);
        if (readmode == READALL && (rv == 1 || (rv == 3 && bytesread)))
            /* EOF is no error here */
            rv = 0;
    }

    if (savehist)
        histsave(&source->line, Xstring(xs, xp), HIST_STORE, false);

    ccp = cp = Xclose(xs, xp);
    expanding = false;
    XinitN(xs, 128, ATEMP);
    if (intoarray) {
        vp = global(*wp);
        subarray = last_lookup_was_array;
        if (vp->flag & RDONLY) {
        c_read_splitro:
            kwarnf(KWF_BIERR | KWF_TWOMSG | KWF_NOERRNO, Tread_only, *wp);
        c_read_spliterr:
            rv = 2;
            afree(cp, ATEMP);
            goto c_read_out;
        }
        /* counter for array index */
        if (subarray)
            idx = arrayindex(vp);
        /* exporting an array is currently pointless */
        unset(vp, subarray ? 0 : 1);
    }
    if (!aschars) {
        /* skip initial IFS whitespace */
        while (bytesread && is_ifsws(*ccp)) {
            ++ccp;
            --bytesread;
        }
        /* trim trailing IFS whitespace */
        while (bytesread && is_ifsws(ccp[bytesread - 1])) {
            --bytesread;
        }
    }
c_read_splitloop:
    xp = Xstring(xs, xp);
    /* generate next word */
    if (!bytesread) {
        /* no more input */
        if (intoarray)
            goto c_read_splitdone;
        /* zero out next parameters */
        goto c_read_gotword;
    }
    if (aschars) {
        bytesleft = ez_mbtoc(NULL, ccp);
        if (!bytesleft) {
            /* got a NUL byte */
            Xput(xs, xp, '2');
            Xput(xs, xp, '#');
            Xput(xs, xp, '0');
            ++ccp;
            --bytesread;
            goto c_read_gotword;
        }
        Xput(xs, xp, '1');
        Xput(xs, xp, '#');
        while (bytesleft && bytesread) {
            *xp++ = *ccp++;
            --bytesleft;
            --bytesread;
        }
        goto c_read_gotword;
    }

    if (!intoarray && wp[1] == NULL)
        lastparmmode = true;

c_read_splitlast:
    /* copy until IFS character */
    while (bytesread) {
        char ch;

        ch = *ccp;
        if (expanding) {
            expanding = false;
            goto c_read_splitcopy;
        } else if (ctype(ch, C_IFS)) {
            break;
        } else if (!rawmode && ch == '\\') {
            expanding = true;
        } else {
        c_read_splitcopy:
            Xcheck(xs, xp);
            Xput(xs, xp, ch);
        }
        ++ccp;
        --bytesread;
    }
    xsave = Xsavepos(xs, xp);
    /* copy word delimiter: IFSWS+IFS,IFSWS */
    expanding = false;
    while (bytesread) {
        char ch;

        ch = *ccp;
        if (!ctype(ch, C_IFS))
            break;
        if (lastparmmode && !expanding && !rawmode && ch == '\\') {
            expanding = true;
        } else {
            Xcheck(xs, xp);
            Xput(xs, xp, ch);
        }
        ++ccp;
        --bytesread;
        if (expanding)
            continue;
        if (!ctype(ch, C_IFSWS))
            break;
    }
    while (bytesread && is_ifsws(*ccp)) {
        Xcheck(xs, xp);
        Xput(xs, xp, *ccp);
        ++ccp;
        --bytesread;
    }
    /* if no more parameters, rinse and repeat */
    if (lastparmmode && bytesread) {
        lastparmused = true;
        goto c_read_splitlast;
    }
    /* get rid of the delimiter unless we pack the rest */
    if (!lastparmused)
        xp = Xrestpos(xs, xp, xsave);
c_read_gotword:
    Xput(xs, xp, '\0');
    if (intoarray) {
        if (subarray) {
            /* array element passed, accept first read */
            if (vq) {
                bi_errorf("nested arrays not yet supported");
                goto c_read_spliterr;
            }
            vq = vp;
            if (idx)
                /* [0] doesn't */
                vq->flag |= AINDEX;
        } else {
            vq = arraysearch(vp, idx);
            idx = qiMO(k32, K32_FM, idx, +, 1U);
        }
    } else {
        vq = global(*wp);
        /* must be checked before exporting */
        if (vq->flag & RDONLY)
            goto c_read_splitro;
        if (Flag(FEXPORT))
            typeset(*wp, EXPORT, 0, 0, 0);
    }
    if (!setstr(vq, Xstring(xs, xp), QSH_RETURN_ERROR))
        goto c_read_spliterr;
    if (aschars) {
        setint_v(vq, vq, false);
        /* protect from UTFMODE changes */
        vq->type = 0;
    }
    if (intoarray || *++wp != NULL)
        goto c_read_splitloop;

c_read_splitdone:
    /* free up */
    afree(cp, ATEMP);

c_read_out:
    afree(allocd, ATEMP);
    Xfree(xs, xp);
    return (rv == 3 ? qsh_sigmask(SIGALRM) : rv);
#undef is_ifsws
}
