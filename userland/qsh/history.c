/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"
#include "qsh_hash.h"


/* Persistent-history file machinery lives in history_file.c.  These
 * extern declarations let history.c call the file-side functions and
 * see the shared state. */

static int hist_execute(char *, Area *);
static char **hist_get(const char *, bool, bool);
static char **hist_get_oldest(void);

/* Shared with history_file.c — used by hist_init/hist_finish to set
 * up persistent storage and by histload to insert loaded lines. */
bool hstarted; /* set after hist_init() called */
Source *hist_source;

/* HISTSIZE default: size of saved history, persistent or standard */
#define QSH_DEFHISTSIZE 2047
/* maximum considered size of persistent history file */
#define QSH_MAXHISTFSIZE ((off_t)1048576 * 96)

/* hidden option */
#define HIST_DISCARD 5

int
c_fc(const char **wp)
{
    struct shf *shf;
    struct temp *tf;
    bool gflag = false, lflag = false, nflag = false, rflag = false, sflag = false;
    int optc;
    const char *p, *first = NULL, *last = NULL;
    char **hfirst, **hlast, **hp, *editor = NULL;

    if (!Flag(FTALKING_I)) {
        bi_errorf("history %ss not available", Tfunction);
        return (1);
    }

    while ((optc = qsh_getopt(wp, &builtin_opt, "e:glnrs0,1,2,3,4,5,6,7,8,9,")) != -1)
        switch (optc) {

        case 'e':
            p = builtin_opt.optarg;
            if (qsh_isdash(p))
                sflag = true;
            else {
                size_t len = strlen(p);

                /* almost certainly not overflowing */
                editor = alloc(len + 6U, ATEMP);
                memcpy(editor, p, len);
                memcpy(editor + len, Tspdollaru, 6U);
            }
            break;

        /* non-AT&T ksh */
        case 'g':
            gflag = true;
            break;

        case 'l':
            lflag = true;
            break;

        case 'n':
            nflag = true;
            break;

        case 'r':
            rflag = true;
            break;

        /* POSIX version of -e - */
        case 's':
            sflag = true;
            break;

        /* kludge city - accept -num as -- -num (kind of) */
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            p = shf_smprintf("-%c%s", optc, builtin_opt.optarg);
            if (!first)
                first = p;
            else if (!last)
                last = p;
            else {
                bi_errorf(Ttoo_many_args);
                return (1);
            }
            break;

        case '?':
            return (1);
        }
    wp += builtin_opt.optind;

    /* Substitute and execute command */
    if (sflag) {
        char *pat = NULL, *rep = NULL, *line;

        if (editor || lflag || nflag || rflag) {
            bi_errorf("can't use -e, -l, -n, -r with -s (-e -)");
            return (1);
        }

        /* Check for pattern replacement argument */
        if (*wp && **wp && (p = cstrchr(*wp + 1, '='))) {
            strdupx(pat, *wp, ATEMP);
            rep = pat + (p - *wp);
            *rep++ = '\0';
            wp++;
        }
        /* Check for search prefix */
        if (!first && (first = *wp))
            wp++;
        if (last || *wp) {
            bi_errorf(Ttoo_many_args);
            return (1);
        }

        hp = first ? hist_get(first, false, false) : hist_get_newest(false);
        if (!hp)
            return (1);
        /* hist_replace */
        if (!pat)
            strdupx(line, *hp, ATEMP);
        else {
            char *s, *s1;
            size_t len, pat_len, rep_len;
            XString xs;
            char *xp;
            bool any_subst = false;

            pat_len = strlen(pat);
            rep_len = strlen(rep);
            Xinit(xs, xp, 128, ATEMP);
            for (s = *hp; (s1 = ucstrstr(s, pat)) && (!any_subst || gflag); s = s1 + pat_len) {
                any_subst = true;
                len = s1 - s;
                XcheckN(xs, xp, len + rep_len);
                /*; first part */
                memcpy(xp, s, len);
                xp += len;
                /* replacement */
                memcpy(xp, rep, rep_len);
                xp += rep_len;
            }
            if (!any_subst) {
                bi_errorf(Tbadsubst);
                return (1);
            }
            len = strlen(s) + 1;
            XcheckN(xs, xp, len);
            memcpy(xp, s, len);
            xp += len;
            line = Xclose(xs, xp);
        }
        return (hist_execute(line, ATEMP));
    }

    if (editor && (lflag || nflag)) {
        bi_errorf("can't use -l, -n with -e");
        return (1);
    }

    if (!first && (first = *wp))
        wp++;
    if (!last && (last = *wp))
        wp++;
    if (*wp) {
        bi_errorf(Ttoo_many_args);
        return (1);
    }
    if (!first) {
        hfirst = lflag ? hist_get("-16", true, true) : hist_get_newest(false);
        if (!hfirst)
            return (1);
        /* can't fail if hfirst didn't fail */
        hlast = hist_get_newest(false);
    } else {
        /*
         * POSIX says not an error if first/last out of bounds
         * when range is specified; AT&T ksh and pdksh allow out
         * of bounds for -l as well.
         */
        hfirst = hist_get(first, ((bool)(lflag || last)), lflag);
        if (!hfirst)
            return (1);
        hlast = last ? hist_get(last, true, lflag) : (lflag ? hist_get_newest(false) : hfirst);
        if (!hlast)
            return (1);
    }
    if (hfirst > hlast) {
        char **temp;

        temp = hfirst;
        hfirst = hlast;
        hlast = temp;
        /* POSIX */
        rflag = !rflag;
    }

    /* List history */
    if (lflag) {
        char *s, *t;

        for (hp = rflag ? hlast : hfirst; hp >= hfirst && hp <= hlast; hp += rflag ? -1 : 1) {
            if (!nflag)
                shf_fprintf(shl_stdout, Tf_lu,
                            (unsigned long)hist_source->line - (unsigned long)(histptr - hp));
            shf_putc('\t', shl_stdout);
            /* print multi-line commands correctly */
            s = *hp;
            while ((t = ucstrchr(s, '\n'))) {
                *t = '\0';
                shf_fprintf(shl_stdout, "%s\n\t", s);
                *t++ = '\n';
                s = t;
            }
            shf_fprintf(shl_stdout, Tf_sN, s);
        }
        shf_flush(shl_stdout);
        return (0);
    }

    /* Run editor on selected lines, then run resulting commands */

    tf = maketemp(ATEMP, TT_HIST_EDIT, &e->temps);
    if (!(shf = tf->shf)) {
        kwarnf0(KWF_BIERR, Tf_temp, Tcreate, tf->tffn);
        return (1);
    }
    for (hp = rflag ? hlast : hfirst; hp >= hfirst && hp <= hlast; hp += rflag ? -1 : 1)
        shf_fprintf(shf, Tf_sN, *hp);
    if (shf_close(shf) == -1) {
        kwarnf0(KWF_BIERR, Tf_temp, Twrite, tf->tffn);
        return (1);
    }

    /* Ignore setstr errors here (arbitrary) */
    setstr(local("_", false), tf->tffn, QSH_RETURN_ERROR);

    if ((optc = command(editor ? editor : TFCEDIT_dollaru, 0)))
        return (optc);

    {
        struct stat statb;
        XString xs;
        char *xp;
        ssize_t n;

        if (!(shf = shf_open(tf->tffn, O_RDONLY, 0, 0))) {
            kwarnf0(KWF_BIERR, Tf_temp, Topen, tf->tffn);
            return (1);
        }

        if (stat(tf->tffn, &statb) < 0)
            n = 128;
        else if ((off_t)statb.st_size > QSH_MAXHISTFSIZE) {
            bi_errorf(Tf_toolarge, Tfile, (unsigned long)statb.st_size);
            goto errout;
        } else
            n = (size_t)statb.st_size + 1;
        Xinit(xs, xp, n, hist_source->areap);
        while ((n = shf_read(xp, Xnleft(xs, xp), shf)) > 0) {
            xp += n;
            if (Xnleft(xs, xp) <= 0)
                XcheckN(xs, xp, Xlength(xs, xp));
        }
        if (n < 0) {
            kwarnf1(KWF_VERRNO | KWF_BIERR, shf_errno(shf), Tf_temp, Tread, tf->tffn);
        errout:
            shf_close(shf);
            return (1);
        }
        shf_close(shf);
        *xp = '\0';
        strip_nuls(Xstring(xs, xp), Xlength(xs, xp));
        return (hist_execute(Xstring(xs, xp), hist_source->areap));
    }
}

/* save cmd in history, execute cmd (cmd gets afree’d) */
static int
hist_execute(char *cmd, Area *areap)
{
    static int last_line = -1;

    /* Back up over last histsave */
    if (histptr >= history && last_line != hist_source->line) {
        hist_source->line--;
        afree(*histptr, APERM);
        histptr--;
        last_line = hist_source->line;
    }

    histsave(&hist_source->line, cmd, HIST_STORE, true);
    /* now *histptr == cmd without all trailing newlines */
    afree(cmd, areap);
    cmd = *histptr;
    /* pdksh says POSIX doesn’t say this is done, testsuite needs it */
    shellf(Tf_sN, cmd);

    /*-
     * Commands are executed here instead of pushing them onto the
     * input 'cause POSIX says the redirection and variable assignments
     * in
     *  X=y fc -e - 42 2> /dev/null
     * are to effect the repeated commands environment.
     */
    return (command(cmd, 0));
}

/*
 * get pointer to history given pattern
 * pattern is a number or string
 */
static char **
hist_get(const char *str, bool approx, bool allow_cur)
{
    char **hp = NULL;
    int n;

    if (getn(str, &n)) {
        hp = histptr + (n < 0 ? n : (n - hist_source->line));
        if ((size_t)hp < (size_t)history) {
            if (approx)
                hp = hist_get_oldest();
            else {
                bi_errorf(Tf_sD_s, str, Tnot_in_history);
                hp = NULL;
            }
        } else if ((size_t)hp > (size_t)histptr) {
            if (approx)
                hp = hist_get_newest(allow_cur);
            else {
                bi_errorf(Tf_sD_s, str, Tnot_in_history);
                hp = NULL;
            }
        } else if (!allow_cur && hp == histptr) {
            bi_errorf(Tf_sD_s, str, "invalid range");
            hp = NULL;
        }
    } else {
        bool anchd = *str == '?' ? (++str, false) : true;

        /* the -1 is to avoid the current fc command */
        if ((n = findhist(histptr - history - 1, str, false, anchd)) < 0)
            bi_errorf(Tf_sD_s, str, Tnot_in_history);
        else
            hp = &history[n];
    }
    return (hp);
}

/* Return a pointer to the newest command in the history */
char **
hist_get_newest(bool allow_cur)
{
    if (histptr < history || (!allow_cur && histptr == history)) {
        bi_errorf("no history (yet)");
        return (NULL);
    }
    return (allow_cur ? histptr : histptr - 1);
}

/* Return a pointer to the oldest command in the history */
static char **
hist_get_oldest(void)
{
    if (histptr <= history) {
        bi_errorf("no history (yet)");
        return (NULL);
    }
    return (history);
}

/* current position in history[] */
char **current;  /* shared with history_file.c */

/*
 * Return the current position.
 */
char **
histpos(void)
{
    return (current);
}

int
histnum(int n)
{
    int last = histptr - history;

    if (n < 0 || n >= last) {
        current = histptr;
        return (last);
    } else {
        current = &history[n];
        return (n);
    }
}

/*
 * This will become unnecessary if hist_get is modified to allow
 * searching from positions other than the end, and in either
 * direction.
 */
int
findhist(int start, const char *str, bool fwd, bool anchored)
{
    char **hp;
    int maxhist = histptr - history;
    int incr = fwd ? 1 : -1;
    size_t len = strlen(str);

    if (start < 0 || start >= maxhist)
        start = maxhist;

    hp = &history[start];
    for (; hp >= history && hp <= histptr; hp += incr)
        if ((anchored && strncmp(*hp, str, len) == 0) || (!anchored && vstrstr(*hp, str)))
            return (hp - history);

    return (-1);
}

/*
 * set history; this means reallocating the dataspace
 */
void
sethistsize(qsh_ari_t n)
{
    if (n > 65535)
        n = 65535;
    if (n > 0 && n != histsize) {
        int cursize = histptr - history;

        /* save most recent history */
        if (n < cursize) {
            memmove(history, histptr - n + 1, n * sizeof(char *));
            cursize = n - 1;
        }

        history = aresize2(history, n, sizeof(char *), APERM);

        histsize = n;
        histptr = history + cursize;
    }
}

/*
 * initialise the history vector
 */
void
init_histvec(void)
{
    if (history == (char **)NULL) {
        histsize = QSH_DEFHISTSIZE;
        history = alloc2(histsize, sizeof(char *), APERM);
        histptr = history - 1;
    }
}

/*
 * It turns out that there is a lot of ghastly hackery here
 */

/*
 * save command in history
 */
void
histsave(int *lnp, const char *cmd, int svmode, bool ignoredups)
{
    static char *enqueued = NULL;
    char **hp, *c;
    const char *ccp;

    if (svmode == HIST_DISCARD) {
        afree(enqueued, APERM);
        enqueued = NULL;
        return;
    }

    if (svmode == HIST_APPEND) {
        if (!enqueued)
            svmode = HIST_STORE;
    } else if (enqueued) {
        c = enqueued;
        enqueued = NULL;
        --*lnp;
        histsave(lnp, c, HIST_STORE, true);
        afree(c, APERM);
    }

    if (svmode == HIST_FLUSH)
        return;

    ccp = strnul(cmd);
    while (ccp > cmd && ccp[-1] == '\n')
        --ccp;
    strndupx(c, cmd, ccp - cmd, APERM);

    if (svmode != HIST_APPEND) {
        if (ignoredups && histptr >= history && !strcmp(c, *histptr)
        ) {
            afree(c, APERM);
            return;
        }
        ++*lnp;
    }

    if (svmode == HIST_QUEUE || svmode == HIST_APPEND) {
        size_t nenq, ncmd;

        if (!enqueued) {
            if (*c)
                enqueued = c;
            else
                afree(c, APERM);
            return;
        }

        nenq = strlen(enqueued);
        ncmd = strlen(c);
        enqueued = aresize1(enqueued, nenq + 1, ncmd + 1, APERM);
        enqueued[nenq] = '\n';
        memcpy(enqueued + nenq + 1, c, ncmd + 1);
        afree(c, APERM);
        return;
    }

    hp = histptr;

    if (++hp >= history + histsize) {
        /* remove oldest command */
        afree(*history, APERM);
        for (hp = history; hp < history + histsize - 1; hp++)
            hp[0] = hp[1];
    }
    *hp = c;
    histptr = hp;
}

/*
 * Write history data to a file nominated by HISTFILE;
 * if HISTFILE is unset then history still happens, but
 * the data is not written to a file. All copies of ksh
 * looking at the file will maintain the same history.
 * This is ksh behaviour.
 *
 * This stuff uses mmap()
 *
 * This stuff is so totally broken it must eventually be
 * redesigned, without mmap, better checks, support for
 * larger files, etc. and handle partially corrupted files
 */

