/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Job-control / signal builtins: jobs, fg, bg, kill, suspend.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

struct kill_info {
    int num_width;
    int name_width;
};

static void kill_fmt_entry(char *, size_t, unsigned int, const void *);

int
c_jobs(const char **wp)
{
    int optc, flag = 0, nflag = 0, rv = 0;

    while ((optc = qsh_getopt(wp, &builtin_opt, "lpnz")) != -1)
        switch (optc) {
        case 'l':
            flag = 1;
            break;
        case 'p':
            flag = 2;
            break;
        case 'n':
            nflag = 1;
            break;
        case 'z':
            /* debugging: print zombies */
            nflag = -1;
            break;
        case '?':
            return (1);
        }
    wp += builtin_opt.optind;
    if (!*wp) {
        if (j_jobs(NULL, flag, nflag))
            rv = 1;
    } else {
        for (; *wp; wp++)
            if (j_jobs(*wp, flag, nflag))
                rv = 1;
    }
    return (rv);
}

/* format a single kill item */
static void
kill_fmt_entry(char *buf, size_t buflen, unsigned int i, const void *arg)
{
    const struct kill_info *ki = (const struct kill_info *)arg;

    i++;
    shf_snprintf(buf, buflen, "%*u %*s %s", ki->num_width, i, ki->name_width, sigtraps[i].name,
                 sigtraps[i].mess);
}

int
c_kill(const char **wp)
{
    Trap *t = NULL;
    const char *p;
    bool lflag = false;
    int i, n, rv, sig;

    /* assume old style options if -digits or -UPPERCASE */
    if ((p = wp[1]) && isch(*p, '-') && ctype(p[1], C_DIGIT | C_UPPER)) {
        ++p;
        if (!(t = gettrap(p, false, false))) {
            kwarnf(KWF_BIERR | KWF_TWOMSG | KWF_NOERRNO, Tbad_sig, p);
            return (1);
        }
        i = (wp[2] && strcmp(wp[2], "--") == 0) ? 3 : 2;
    } else {
        int optc;

        while ((optc = qsh_getopt(wp, &builtin_opt, "ls:")) != -1)
            switch (optc) {
            case 'l':
                lflag = true;
                break;
            case 's':
                if (!(t = gettrap(builtin_opt.optarg, true, false))) {
                    kwarnf(KWF_BIERR | KWF_TWOMSG | KWF_NOERRNO, Tbad_sig, builtin_opt.optarg);
                    return (1);
                }
                break;
            case '?':
                return (1);
            }
        i = builtin_opt.optind;
    }
    if ((lflag && t) || (!wp[i] && !lflag)) {
        shf_puts("usage:\tkill [-s signame | -signum | -signame]"
                 " { job | pid | pgrp } ...\n"
                 "\tkill -l [exit_status ...]\n",
                 shl_out);
        bi_unwind(1);
        return (1);
    }

    if (lflag) {
        if (wp[i]) {
            for (; wp[i]; i++) {
                if (!bi_getn(wp[i], &n))
                    return (1);
#if (qsh_NSIG <= 128)
                if (n > 128 && n < 128 + qsh_NSIG)
                    n -= 128;
#endif
                if (n > 0 && n < qsh_NSIG)
                    shprintf(Tf_sN, sigtraps[n].name);
                else
                    shprintf(Tf_dN, n);
            }
        } else if (Flag(FPOSIX)) {
            n = 1;
            while (n < qsh_NSIG) {
                shf_puts(sigtraps[n].name, shl_stdout);
                ++n;
                shf_putc(n == qsh_NSIG ? '\n' : ' ', shl_stdout);
            }
        } else {
            ssize_t w, mess_cols = 0, mess_octs = 0;
            int j = qsh_NSIG - 1;
            struct kill_info ki = {0, 0};
            struct columnise_opts co;

            do {
                ki.num_width++;
            } while ((j /= 10));

            for (j = 1; j < qsh_NSIG; j++) {
                w = strlen(sigtraps[j].name);
                if (w > ki.name_width)
                    ki.name_width = w;
                w = strlen(sigtraps[j].mess);
                if (w > mess_octs)
                    mess_octs = w;
                w = utf_mbswidth(sigtraps[j].mess);
                if (w > mess_cols)
                    mess_cols = w;
            }

            co.shf = shl_stdout;
            co.linesep = '\n';
            co.prefcol = co.do_last = true;

            print_columns(&co, (unsigned int)(qsh_NSIG - 1), kill_fmt_entry, (void *)&ki,
                          ki.num_width + 1 + ki.name_width + 1 + mess_octs,
                          ki.num_width + 1 + ki.name_width + 1 + mess_cols);
        }
        return (0);
    }
    rv = 0;
    sig = t ? t->signal : SIGTERM;
    for (; (p = wp[i]); i++) {
        if (*p == '%') {
            if (j_kill(p, sig))
                rv = 1;
        } else if (!getn(p, &n)) {
            bi_errorf(Tf_sD_s, p, "arguments must be jobs or process IDs");
            rv = 1;
        } else {
            if (kill(n, sig) < 0) {
                bi_errorf(Tf_sD_s, p, cstrerror(errno));
                rv = 1;
            }
        }
    }
    return (rv);
}
