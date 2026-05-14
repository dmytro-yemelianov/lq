/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Output / pathname builtins: pwd, print/echo, realpath, id.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"
#include <unistd.h>

static const char *s_ptr;
static int s_get(void);
static void s_put(int);

int
c_pwd(const char **wp)
{
    int optc;
    bool physical = ((bool)(Flag(FPHYSICAL)));
    const char *p;
    char *allocd = NULL;

    while ((optc = qsh_getopt(wp, &builtin_opt, "LP")) != -1)
        switch (optc) {
        case 'L':
            physical = false;
            break;
        case 'P':
            physical = true;
            break;
        case '?':
            return (1);
        }
    wp += builtin_opt.optind;

    if (wp[0]) {
        bi_errorf(Ttoo_many_args);
        return (1);
    }
    p = current_wd[0] ? (physical ? allocd = do_realpath(current_wd) : current_wd) : NULL;
    /* LINTED use of access */
    if (p && access(p, R_OK) < 0)
        p = NULL;
    if (!p && !(p = qsh_getwd())) {
        bi_errorf(Tf_sD_s, "can't determine current directory", cstrerror(errno));
        return (1);
    }
    shprintf(Tf_sN, p);
    afree(allocd, ATEMP);
    return (0);
}

/*
 * id — print process credentials.  Minimal form for now:
 *   uid=<n> gid=<n>           when ruid==euid, rgid==egid
 *   uid=<n> gid=<n> euid=<m>  when euid differs (after seteuid, suid bit)
 *   uid=<n> gid=<n> egid=<m>  similar for egid
 *
 * No name lookup, no -u/-g/-G flags, no group list — those land when we
 * need them.  Backed by the new posix_id.c which MsgSends to procmgr
 * for the live values, so it tracks setuid()/setgid().
 */
int
c_id(const char **wp)
{
    uid_t ruid, euid;
    gid_t rgid, egid;

    if (wp[1]) {
        bi_errorf("usage: id");
        return (1);
    }

    ruid = getuid();
    euid = geteuid();
    rgid = getgid();
    egid = getegid();

    shprintf("uid=%u gid=%u", (unsigned)ruid, (unsigned)rgid);
    if (euid != ruid)
        shprintf(" euid=%u", (unsigned)euid);
    if (egid != rgid)
        shprintf(" egid=%u", (unsigned)egid);
    shprintf("\n");
    return (0);
}

int
c_print(const char **wp)
{
    int c;
    const char *s;
    char *xp;
    XString xs;
    struct {
        /* storage for columnisation */
        XPtrV words;
        /* temporary storage for a wide character */
        qsh_ari_t wc;
        /* output file descriptor (if any) */
        int fd;
        /* output word separator */
        char ws;
        /* output line separator */
        char ls;
        /* output a trailing line separator? */
        bool nl;
        /* expand backslash sequences? */
        bool exp;
        /* columnise output? */
        bool col;
        /* print to history instead of file descriptor / stdout? */
        bool hist;
        /* print words as wide characters? */
        bool chars;
        /* writing to a coprocess (SIGPIPE blocked)? */
        bool coproc;
        bool copipe;
    } po;

    memset(&po, 0, sizeof(po));
    po.fd = 1;
    po.ws = ' ';
    po.ls = '\n';
    po.nl = true;

    if (wp[0][0] == 'e') {
        /* "echo" builtin */
        if (Flag(FPOSIX) || Flag(FSH) || as_builtin) {
            /* BSD "echo" cmd, Debian Policy 10.4 compliant */
            ++wp;
        bsd_echo:
            if (*wp && !strcmp(*wp, Tdn)) {
                po.nl = false;
                ++wp;
            }
            po.exp = false;
        } else {
            bool new_exp, new_nl = true;

            /*-
             * compromise between various historic echos: only
             * recognise -Een if they appear in arguments with
             * no illegal options; e.g. echo -nq outputs '-nq'
             */
            /* compromise on -e enabled by default */
            new_exp = true;
            goto print_tradparse_beg;

        print_tradparse_arg:
            if ((s = *wp) && *s++ == '-' && *s) {
            print_tradparse_ch:
                switch ((c = *s++)) {
                case 'E':
                    new_exp = false;
                    goto print_tradparse_ch;
                case 'e':
                    new_exp = true;
                    goto print_tradparse_ch;
                case 'n':
                    new_nl = false;
                    goto print_tradparse_ch;
                case '\0':
                print_tradparse_beg:
                    po.exp = new_exp;
                    po.nl = new_nl;
                    ++wp;
                    goto print_tradparse_arg;
                }
            }
        }
    } else {
        /* "print" builtin */
        const char *opts = "AcelNnpRrsu,";
        const char *emsg;

        po.exp = true;

        while ((c = qsh_getopt(wp, &builtin_opt, opts)) != -1)
            switch (c) {
            case 'A':
                po.chars = true;
                break;
            case 'c':
                po.col = true;
                break;
            case 'e':
                po.exp = true;
                break;
            case 'l':
                po.ws = '\n';
                break;
            case 'N':
                po.ws = '\0';
                po.ls = '\0';
                break;
            case 'n':
                po.nl = false;
                break;
            case 'p':
                if ((po.fd = coproc_getfd(W_OK, &emsg)) < 0) {
                    bi_errorf("%s: %s", Tdp, emsg);
                    return (1);
                }
                break;
            case 'R':
                /* fake BSD echo but don't reset other flags */
                wp += builtin_opt.optind;
                goto bsd_echo;
            case 'r':
                po.exp = false;
                break;
            case 's':
                po.hist = true;
                break;
            case 'u':
                if (!*(s = builtin_opt.optarg))
                    po.fd = 0;
                else if ((po.fd = check_fd(s, W_OK, &emsg)) < 0) {
                    bi_errorf("-u%s: %s", s, emsg);
                    return (1);
                }
                break;
            case '?':
                return (1);
            }

        if (!(builtin_opt.info & GI_MINUSMINUS)) {
            /* treat a lone "-" like "--" */
            if (wp[builtin_opt.optind] && qsh_isdash(wp[builtin_opt.optind]))
                builtin_opt.optind++;
        }
        wp += builtin_opt.optind;
    }

    if (po.col) {
        if (*wp == NULL)
            return (0);

        XPinit(po.words, 16);
    }

    Xinit(xs, xp, 128, ATEMP);

    if (*wp == NULL)
        goto print_no_arg;
print_read_arg:
    if (po.chars) {
        while (*wp != NULL) {
            s = *wp++;
            if (*s == '\0')
                break;
            if (!evaluate(s, &po.wc, QSH_RETURN_ERROR, true))
                return (1);
            XcheckN(xs, xp, 4);
            xp += ez_ctomb(xp, po.wc);
        }
    } else {
        s = *wp++;
        while ((c = *s++) != '\0') {
            XcheckN(xs, xp, 4);
            if (po.exp && c == '\\') {
                s_ptr = s;
                c = unbksl(false, s_get, s_put);
                s = s_ptr;
                if (c == -1) {
                    /* rejected by generic function */
                    switch ((c = *s++)) {
                    case 'c':
                        po.nl = false;
                        /* AT&T brain damage */
                        continue;
                    case '\0':
                        --s;
                        c = '\\';
                        break;
                    default:
                        Xput(xs, xp, '\\');
                    }
                } else if ((unsigned int)c > 0xFF) {
                    /* generic function returned UCS */
                    xp += utf_wctomb(xp, c - 0x100);
                    continue;
                }
            }
            Xput(xs, xp, c);
        }
    }
    if (po.col) {
        Xput(xs, xp, '\0');
        XPput(po.words, Xclose(xs, xp));
        Xinit(xs, xp, 128, ATEMP);
    }
    if (*wp != NULL) {
        if (!po.col)
            Xput(xs, xp, po.ws);
        goto print_read_arg;
    }
    if (po.col) {
        size_t w = XPsize(po.words);
        struct columnise_opts co;

        XPput(po.words, NULL);
        co.shf = shf_sopen(NULL, 128, SHF_WR | SHF_DYNAMIC, NULL);
        co.linesep = po.ls;
        co.prefcol = co.do_last = false;
        pr_list(&co, (char **)XPptrv(po.words));
        while (w--)
            afree(XPptrv(po.words)[w], ATEMP);
        XPfree(po.words);
        w = co.shf->wp - co.shf->buf;
        XcheckN(xs, xp, w);
        memcpy(xp, co.shf->buf, w);
        xp += w;
        shf_sclose(co.shf);
    }
print_no_arg:
    if (po.nl)
        Xput(xs, xp, po.ls);

    c = 0;
    if (po.hist) {
        Xput(xs, xp, '\0');
        histsave(&source->line, Xstring(xs, xp), HIST_STORE, false);
    } else {
        size_t len = Xlength(xs, xp);

        /*
         * Ensure we aren't killed by a SIGPIPE while writing to
         * a coprocess. AT&T ksh doesn't seem to do this (seems
         * to just check that the co-process is alive which is
         * not enough).
         */
        if (coproc.write >= 0 && coproc.write == po.fd) {
            po.coproc = true;
            po.copipe = block_pipe();
        } else
            po.coproc = po.copipe = false;

        s = Xstring(xs, xp);
        while (len > 0) {
            ssize_t nwritten;

            if ((nwritten = write(po.fd, s, len)) < 0) {
                if (errno == EINTR) {
                    if (po.copipe)
                        restore_pipe();
                    /* give the user a chance to ^C out */
                    intrcheck();
                    /* interrupted, try again */
                    if (po.coproc)
                        po.copipe = block_pipe();
                    continue;
                }
                bi_errorf(Tf_sD_s, Twrite, cstrerror(errno));
                c = 1;
                break;
            }
            s += nwritten;
            len -= nwritten;
        }
        if (po.copipe)
            restore_pipe();
    }
    Xfree(xs, xp);

    return (c);
}

static int
s_get(void)
{
    return (ord(*s_ptr++));
}

static void
s_put(int c QSH_A_UNUSED)
{
    --s_ptr;
}

int
c_realpath(const char **wp)
{
    int rv = 1;
    char *buf;

    /* skip argv[0] */
    ++wp;
    if (wp[0] && !strcmp(wp[0], "--"))
        /* skip "--" (options separator) */
        ++wp;

    /* check for exactly one argument */
    if (wp[0] == NULL || wp[1] != NULL)
        bi_errorf(Tsynerr);
    else if ((buf = do_realpath(wp[0])) == NULL) {
        rv = errno;
        bi_errorf(Tf_sD_s, wp[0], cstrerror(rv));
        if ((unsigned int)rv > 255)
            rv = 255;
    } else {
        shprintf(Tf_sN, buf);
        afree(buf, ATEMP);
        rv = 0;
    }

    return (rv);
}
