/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Time / exec / mknod builtins.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

#define TF_NOARGS BIT(0)
#define TF_NOREAL BIT(1) /* don't report real time */
#define TF_POSIX  BIT(2) /* report in POSIX format */

static void
c_times_i(int what)
{
    struct rusage usage;

    if (qsh_getrusage(what, &usage))
        kwarnf(KWF_BIERR | KWF_ONEMSG, "getrusage");
    shf_fprintf(shl_stdout, "%ldm%02d.%02ds %ldm%02d.%02ds\n", (long)(usage.ru_utime.tv_sec / 60),
                (int)(usage.ru_utime.tv_sec % 60), (int)(usage.ru_utime.tv_usec / 10000),
                (long)(usage.ru_stime.tv_sec / 60), (int)(usage.ru_stime.tv_sec % 60),
                (int)(usage.ru_stime.tv_usec / 10000));
}

int
c_times(const char **wp QSH_A_UNUSED)
{
    c_times_i(RUSAGE_SELF);
    c_times_i(RUSAGE_CHILDREN);
    return (0);
}

static void
p_time_psx(struct timeval *tv, const char *prefix)
{
    shf_fprintf(shl_out, "%s%ld.%02d\n", prefix, (long)(tv->tv_sec), (int)(tv->tv_usec / 10000));
}

static void
p_time_ksh(struct timeval *tv, const char *suffix)
{
    shf_fprintf(shl_out, "%5ldm%02d.%02ds%s", (long)(tv->tv_sec / 60), (int)(tv->tv_sec % 60),
                (int)(tv->tv_usec / 10000), suffix);
}

/*
 * time pipeline (really a statement, not a built-in command)
 */
int
timex(struct op *t, int f, volatile int *xerrok)
{
    int rv = 0, tf = 0;
    struct rusage ru0, ru1, cru0, cru1;
    struct timeval usrtime, systime, tv0, tv1;

    qsh_TIME(tv0);
    if (qsh_getrusage(RUSAGE_SELF, &ru0) || qsh_getrusage(RUSAGE_CHILDREN, &cru0)) {
        kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG, Ttime_getrusage);
        return (125);
    }
    if (t->left) {
        /*
         * Two ways of getting cpu usage of a command: just use t0
         * and t1 (which will get cpu usage from other jobs that
         * finish while we are executing t->left), or get the
         * cpu usage of t->left. AT&T ksh does the former, while
         * pdksh tries to do the later (the j_usrtime hack doesn't
         * really work as it only counts the last job).
         */
        timerclear(&j_usrtime);
        timerclear(&j_systime);
        rv = execute(t->left, f | XTIME, xerrok);
        if (t->left->type == TCOM)
            tf |= t->left->str[0];
        qsh_TIME(tv1);
        if (qsh_getrusage(RUSAGE_SELF, &ru1) || qsh_getrusage(RUSAGE_CHILDREN, &cru1)) {
            kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG, Ttime_getrusage);
            return (rv);
        }
    } else
        tf = TF_NOARGS;

    if (tf & TF_NOARGS) {
        /* ksh93 - report shell times (shell+kids) */
        tf |= TF_NOREAL;
        timeradd(&ru0.ru_utime, &cru0.ru_utime, &usrtime);
        timeradd(&ru0.ru_stime, &cru0.ru_stime, &systime);
    } else {
        timersub(&ru1.ru_utime, &ru0.ru_utime, &usrtime);
        timeradd(&usrtime, &j_usrtime, &usrtime);
        timersub(&ru1.ru_stime, &ru0.ru_stime, &systime);
        timeradd(&systime, &j_systime, &systime);
    }

    if (tf & TF_POSIX) {
        if (!(tf & TF_NOREAL)) {
            timersub(&tv1, &tv0, &tv1);
            p_time_psx(&tv1, Treal_sp1);
        }
        p_time_psx(&usrtime, Tuser_sp1);
        p_time_psx(&systime, "sys ");
    } else {
        if (!(tf & TF_NOREAL)) {
            timersub(&tv1, &tv0, &tv1);
            p_time_ksh(&tv1, Treal_sp2);
        }
        p_time_ksh(&usrtime, Tuser_sp2);
        p_time_ksh(&systime, " system\n");
    }
    shf_flush(shl_out);

    return (rv);
}

void
timex_hook(struct op *t, char **volatile *app)
{
    char **wp = *app;
    int optc, i, j;
    Getopt opt;

    qsh_getopt_reset(&opt, 0);
    /* start at the start */
    opt.optind = 0;
    while ((optc = qsh_getopt((const char **)wp, &opt, ":p")) != -1)
        switch (optc) {
        case 'p':
            t->str[0] |= TF_POSIX;
            break;
        case '?':
            qsh_getopt_opterr(opt.optarg[0], Ttime, Tunknown_option);
            unwind(LERROR);
        case ':':
            qsh_getopt_opterr(opt.optarg[0], Ttime, Treq_arg);
            unwind(LERROR);
        }
    /* Copy command words down over options. */
    if (opt.optind != 0) {
        for (i = 0; i < opt.optind; i++)
            afree(wp[i], ATEMP);
        for (i = 0, j = opt.optind; (wp[i] = wp[j]); i++, j++)
            ;
    }
    if (!wp[0])
        t->str[0] |= TF_NOARGS;
    *app = wp;
}

/* exec with no args - args case is taken care of in comexec() */
int
c_exec(const char **wp QSH_A_UNUSED)
{
    int i;
    kui sfd;

    if (e->savedfd == NULL)
        return (0);

    /* make sure redirects stay in place */

    /* for ksh, keep file descriptors private (except stdin/out/err)… */
    if (!Flag(FPOSIX) && !Flag(FSH)) {
        for (i = 0; i < NUFILE; i++) {
            if (!(sfd = FDSVNUM(e, i)))
                continue;
            if (sfd > (kui)FDBASE)
                close((int)sfd);
            if (i > 2 && !(e->savedfd[i] & FDICLMASK) && fcntl(i, F_SETFD, FD_CLOEXEC) == -1)
                kwarnf0(KWF_INTERNAL | KWF_WARNING, Tcloexec_failed, "set", i);
        }
    } else {
        /* … but not for POSIX or legacy/kludge sh */
        for (i = 0; i < NUFILE; i++) {
            sfd = FDSVNUM(e, i);
            if (sfd > (kui)FDBASE)
                close((int)sfd);
        }
    }

    e->savedfd = NULL;
    return (0);
}

