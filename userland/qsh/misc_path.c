/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

static const unsigned char *pat_scan(const unsigned char *, const unsigned char *, bool);

char *
do_realpath(const char *upath)
{
    char *xp, *ip, *tp, *ipath, *ldest = NULL;
    XString xs;
    size_t pos, len, ldestlen = 1U;
    ssize_t llen;
    struct stat sb;
    /* max. recursion depth */
    int symlinks = 32;

    if (qsh_abspath(upath)) {
        /* upath is an absolute pathname */
        strdupx(ipath, upath, ATEMP);
    } else {
        const char *tcp;

        /* upath is a relative pathname, prepend cwd */
        if ((tcp = qsh_getwd()) == NULL)
            return (NULL);
        strpathx(ipath, tcp, upath, 1);
    }

    /* ipath and upath are in memory at the same time -> unchecked */
    Xinit(xs, xp, strlen(ip = ipath) + 1, ATEMP);

    /* now jump into the deep of the loop */
    goto beginning_of_a_pathname;

    while (*ip) {
        /* skip slashes in input */
        while (qsh_cdirsep(*ip))
            ++ip;
        if (!*ip)
            break;

        /* get next pathname component from input */
        tp = ip;
        while (*ip && !qsh_cdirsep(*ip))
            ++ip;
        len = ip - tp;

        /* check input for "." and ".." */
        if (tp[0] == '.') {
            if (len == 1)
                /* just continue with the next one */
                continue;
            else if (len == 2 && tp[1] == '.') {
                /* strip off last pathname component */
                /*XXX consider a rooted pathname */
                while (xp > Xstring(xs, xp))
                    if (qsh_cdirsep(*--xp))
                        break;
                /* then continue with the next one */
                continue;
            }
        }

        /* store output position away, then append slash to output */
        pos = Xsavepos(xs, xp);
        /* 1 for the '/' and len + 1 for tp and the NUL from below */
        XcheckN(xs, xp, 1 + len + 1);
        Xput(xs, xp, '/');

        /* append next pathname component to output */
        memcpy(xp, tp, len);
        xp += len;
        *xp = '\0';

        /* lstat the current output, see if it's a symlink */
        if (qsh_lstat(Xstring(xs, xp), &sb)) {
            /* lstat failed */
            if (errno == ENOENT) {
                /* because the pathname does not exist */
                while (qsh_cdirsep(*ip))
                    /* skip any trailing slashes */
                    ++ip;
                /* no more components left? */
                if (!*ip)
                    /* we can still return successfully */
                    break;
                /* more components left? fall through */
            }
            /* not ENOENT or not at the end of ipath */
            goto notfound;
        }

        /* check if we encountered a symlink? */
        if (S_ISLNK(sb.st_mode)) {
            /* reached maximum recursion depth? */
            if (!symlinks--) {
                /* yep, prevent infinite loops */
                errno = ELOOP;
                goto notfound;
            }

            /* need to resize link target buffer? */
            if ((qiHUGE_U)sb.st_size > (qiHUGE_U)(ldestlen - 1U)) {
                /* like notoktoadd() */
                if ((qiHUGE_U)sb.st_size >= (qiHUGE_U)qiSIZE_MAX) {
                    errno = ENAMETOOLONG;
                    goto notfound;
                }
                ldestlen = (size_t)sb.st_size + 1U;
                /* assert(ldestlen <= qiSIZE_MAX) */
                ldest = aresize(ldest, ldestlen, ATEMP);
            }
            /* get symlink(7) target */
            errno = ENAMETOOLONG; /* for overflow case */
            llen = readlink(Xstring(xs, xp), ldest, ldestlen);
            if (llen == -1 || (size_t)llen >= ldestlen)
                /* oops... */
                goto notfound;
            ldest[(size_t)llen] = '\0';

            /*
             * restart if symlink target is an absolute path,
             * otherwise continue with currently resolved prefix
             */
            /* append rest of current input path to link target */
            strpathx(tp, ldest, ip, 0);
            afree(ipath, ATEMP);
            ip = ipath = tp;
            if (!qsh_abspath(ipath)) {
                /* symlink target is a relative path */
                xp = Xrestpos(xs, xp, pos);
            } else
            {
                /* symlink target is an absolute path */
                xp = Xstring(xs, xp);
            beginning_of_a_pathname:
                /* assert: qsh_abspath(ip == ipath) */
                /* assert: xp == xs.beg => start of path */

                /* exactly two leading slashes? (SUSv4 3.266) */
                if (ip[1] == ip[0] && !qsh_cdirsep(ip[2])) {
                    /* keep them, e.g. for UNC pathnames */
                    Xput(xs, xp, '/');
                }
            }
        }
        /* otherwise (no symlink) merely go on */
    }

    /*
     * either found the target and successfully resolved it,
     * or found its parent directory and may create it
     */
    if (Xlength(xs, xp) == 0)
        /*
         * if the resolved pathname is "", make it "/",
         * otherwise do not add a trailing slash
         */
        Xput(xs, xp, '/');
    Xput(xs, xp, '\0');

    /*
     * if source path had a trailing slash, check if target path
     * is not a non-directory existing file
     */
    if (ip > ipath && qsh_cdirsep(ip[-1])) {
        if (stat(Xstring(xs, xp), &sb)) {
            if (errno != ENOENT)
                goto notfound;
        } else if (!S_ISDIR(sb.st_mode)) {
            errno = ENOTDIR;
            goto notfound;
        }
        /* target now either does not exist or is a directory */
    }

    /* return target path */
    afree(ldest, ATEMP);
    afree(ipath, ATEMP);
    return (Xclose(xs, xp));

notfound:
    /* save; freeing memory might trash it */
    symlinks = errno;
    afree(ldest, ATEMP);
    afree(ipath, ATEMP);
    Xfree(xs, xp);
    errno = symlinks;
    return (NULL);
}

/**
 *  Makes a filename into result using the following algorithm.
 *  - make result NULL
 *  - if file starts with '/', append file to result & set cdpathp to NULL
 *  - if file starts with ./ or ../ append cwd and file to result
 *    and set cdpathp to NULL
 *  - if the first element of cdpathp doesn't start with a '/' xx or '.' xx
 *    then cwd is appended to result.
 *  - the first element of cdpathp is appended to result
 *  - file is appended to result
 *  - cdpathp is set to the start of the next element in cdpathp (or NULL
 *    if there are no more elements.
 *  The return value indicates whether a non-null element from cdpathp
 *  was appended to result.
 */
static int
make_path(const char *cwd, const char *file,
          /* pointer to colon-separated list */
          char **cdpathp, XString *xsp, int *phys_pathp)
{
    int rval = 0;
    bool use_cdpath = true;
    char *plist;
    size_t len, plen = 0;
    char *xp = Xstring(*xsp, xp);

    if (!file) {
        file = null;
    }

    if (qsh_abspath(file)) {
        *phys_pathp = 0;
        use_cdpath = false;
    } else {
        if (file[0] == '.') {
            char c = file[1];

            if (c == '.')
                c = file[2];
            if (qsh_cdirsep(c) || c == '\0')
                use_cdpath = false;
        }

        plist = *cdpathp;
        if (!plist)
            use_cdpath = false;
        else if (use_cdpath) {
            char *pend = plist;

            while (*pend && *pend != QSH_PATHSEPC)
                ++pend;
            plen = pend - plist;
            *cdpathp = *pend ? pend + 1 : NULL;
        }

        if ((!use_cdpath || !plen || !qsh_abspath(plist)) && (cwd && *cwd)) {
            len = strlen(cwd);
            XcheckN(*xsp, xp, len);
            memcpy(xp, cwd, len);
            xp += len;
            if (qsh_cdirsep(xp[-1]))
                xp--;
            *xp++ = '/';
        }
        *phys_pathp = Xlength(*xsp, xp);
        if (use_cdpath && plen) {
            XcheckN(*xsp, xp, plen);
            memcpy(xp, plist, plen);
            xp += plen;
            if (qsh_cdirsep(xp[-1]))
                xp--;
            *xp++ = '/';
            rval = 1;
        }
    }

    len = strlen(file) + 1;
    XcheckN(*xsp, xp, len);
    memcpy(xp, file, len);

    if (!use_cdpath)
        *cdpathp = NULL;

    return (rval);
}

/*-
 * Simplify pathnames containing "." and ".." entries.
 *
 * simplify_path(this)          = that
 * /a/b/c/./../d/..         /a/b
 * //./C/foo/bar/../baz         //C/foo/baz
 * /foo/                /foo
 * /foo/../../bar           /bar
 * /foo/./blah/..           /foo
 * .                    .
 * ..                   ..
 * ./foo                foo
 * foo/../../../bar         ../../bar
 * C:/foo/../..             C:/
 * C:.                  C:
 * C:..                 C:..
 * C:foo/../../blah         C:../blah
 *
 * XXX consider a rooted pathname: we cannot really 'cd ..' for
 * pathnames like: '/', 'c:/', '//foo', '//foo/', '/@unixroot/'
 * (no effect), 'c:', 'c:.' (effect is retaining the '../') but
 * we need to honour this throughout the shell
 */
void
simplify_path(char *p)
{
    char *dp, *ip, *sp, *tp;
    size_t len;
    bool needslash;
#define needdot true

    switch (*p) {
    case 0:
        return;
    case '/':
        /* exactly two leading slashes? (SUSv4 3.266) */
        if (p[1] == p[0] && !qsh_cdirsep(p[2])) {
            /* keep them, e.g. for UNC pathnames */
            ++p;
        }
        needslash = true;
        break;
    default:
        needslash = false;
    }
    dp = ip = sp = p;

    while (*ip) {
        /* skip slashes in input */
        while (qsh_cdirsep(*ip))
            ++ip;
        if (!*ip)
            break;

        /* get next pathname component from input */
        tp = ip;
        while (*ip && !qsh_cdirsep(*ip))
            ++ip;
        len = ip - tp;

        /* check input for "." and ".." */
        if (tp[0] == '.') {
            if (len == 1)
                /* just continue with the next one */
                continue;
            else if (len == 2 && tp[1] == '.') {
                /* parent level, but how? (see above) */
                if (qsh_abspath(p))
                    /* absolute path, only one way */
                    goto strip_last_component;
                else if (dp > sp) {
                    /* relative path, with subpaths */
                    needslash = false;
                strip_last_component:
                    /* strip off last pathname component */
                    while (dp > sp)
                        if (qsh_cdirsep(*--dp))
                            break;
                } else {
                    /* relative path, at its beginning */
                    if (needslash)
                        /* or already dotdot-slash'd */
                        *dp++ = '/';
                    /* keep dotdot-slash if not absolute */
                    *dp++ = '.';
                    *dp++ = '.';
                    needslash = true;
                    sp = dp;
                }
                /* then continue with the next one */
                continue;
            }
        }

        if (needslash)
            *dp++ = '/';

        /* append next pathname component to output */
        memmove(dp, tp, len);
        dp += len;

        /* append slash if we continue */
        needslash = true;
        /* try next component */
    }
    if (dp == p) {
        /* empty path -> dot (or slash, when absolute) */
        if (needslash)
            *dp++ = '/';
        else if (needdot)
            *dp++ = '.';
    }
    *dp = '\0';
#undef needdot
}

void
set_current_wd(const char *nwd)
{
    if (nwd == NULL) {
        nwd = qsh_getwd();
        if (nwd == NULL)
            nwd = null;
    }

    afree(current_wd, APERM);
    strdupx(current_wd, nwd, APERM);
}

int
c_cd(const char **wp)
{
    int optc, rv, phys_path;
    bool physical = ((bool)(Flag(FPHYSICAL)));
    /* was a node from cdpath added in? */
    int cdnode;
    /* show where we went?, error for $PWD */
    bool printpath = false, eflag = false;
    struct tbl *pwd_s, *oldpwd_s;
    XString xs;
    char *dir, *allocd = NULL, *tryp, *pwd, *cdpath;

    while ((optc = qsh_getopt(wp, &builtin_opt, "eLP")) != -1)
        switch (optc) {
        case 'e':
            eflag = true;
            break;
        case 'L':
            physical = false;
            break;
        case 'P':
            physical = true;
            break;
        case '?':
            return (2);
        }
    wp += builtin_opt.optind;

    if (Flag(FRESTRICTED)) {
        bi_errorf(Tcant_cd);
        return (2);
    }

    pwd_s = global(TPWD);
    oldpwd_s = global(TOLDPWD);

    if (!wp[0]) {
        /* no arguments; go home */
        if ((dir = str_val(global("HOME"))) == null) {
            bi_errorf("no home directory (HOME not set)");
            return (2);
        }
    } else if (!wp[1]) {
        /* one argument: - or dir */
        if (qsh_isdash(wp[0])) {
            dir = str_val(oldpwd_s);
            if (dir == null) {
                bi_errorf(Tno_OLDPWD);
                return (2);
            }
            printpath = true;
        } else {
            strdupx(allocd, wp[0], ATEMP);
            dir = allocd;
        }
    } else if (!wp[2]) {
        /* two arguments; substitute arg1 in PWD for arg2 */
        size_t ilen, olen, nlen, elen;
        char *cp;

        if (!current_wd[0]) {
            bi_errorf("can't determine current directory");
            return (2);
        }
        /*
         * Substitute arg1 for arg2 in current path. If the first
         * substitution fails because the cd fails we could try to
         * find another substitution. For now, we don't.
         */
        if ((cp = ucstrstr(current_wd, wp[0])) == NULL) {
            bi_errorf(Tbadsubst);
            return (2);
        }
        /*-
         * ilen = part of current_wd before wp[0]
         * elen = part of current_wd after wp[0]
         */
        ilen = cp - current_wd;
        olen = strlen(wp[0]);
        nlen = strlen(wp[1]);
        elen = strlen(current_wd + ilen + olen) + 1;
        dir = allocd = alloc1(ilen + elen, nlen, ATEMP);
        memcpy(dir, current_wd, ilen);
        memcpy(dir + ilen, wp[1], nlen);
        memcpy(dir + ilen + nlen, current_wd + ilen + olen, elen);
        printpath = true;
    } else {
        bi_errorf(Ttoo_many_args);
        return (2);
    }

    /* only a first guess; make_path will enlarge xs if necessary */
    XinitN(xs, 384U, ATEMP);

    cdpath = str_val(global("CDPATH"));
    do {
        cdnode = make_path(current_wd, dir, &cdpath, &xs, &phys_path);
        if (physical)
            rv = chdir(tryp = Xstring(xs, xp) + phys_path);
        else {
            simplify_path(Xstring(xs, xp));
            rv = chdir(tryp = Xstring(xs, xp));
        }
    } while (rv < 0 && cdpath != NULL);

    if (rv < 0) {
        if (cdnode)
            bi_errorf(Tf_sD_s, dir, "bad directory");
        else
            bi_errorf(Tf_sD_s, tryp, cstrerror(errno));
        afree(allocd, ATEMP);
        Xfree(xs, xp);
        return (2);
    }

    rv = 0;

    /* allocd (above) => dir, which is no longer used */
    afree(allocd, ATEMP);
    allocd = NULL;

    /* Clear out tracked aliases with relative paths */
    flushcom(false);

    /*
     * Set OLDPWD (note: unsetting OLDPWD does not disable this
     * setting in AT&T ksh)
     */
    if (current_wd[0])
        /* Ignore failure (happens if read-only or integer) */
        setstr(oldpwd_s, current_wd, QSH_RETURN_ERROR);

    if (!qsh_abspath(Xstring(xs, xp))) {
        pwd = NULL;
    } else if (!physical) {
        goto norealpath_PWD;
    } else if ((pwd = allocd = do_realpath(Xstring(xs, xp))) == NULL) {
        if (eflag)
            rv = 1;
    norealpath_PWD:
        pwd = Xstring(xs, xp);
    }

    /* Set PWD */
    if (pwd) {
        char *ptmp = pwd;

        set_current_wd(ptmp);
        /* Ignore failure (happens if read-only or integer) */
        setstr(pwd_s, ptmp, QSH_RETURN_ERROR);
    } else {
        set_current_wd(null);
        pwd = Xstring(xs, xp);
        /* XXX unset $PWD? */
        if (eflag)
            rv = 1;
    }
    if (printpath || cdnode)
        shprintf(Tf_sN, pwd);

    afree(allocd, ATEMP);
    Xfree(xs, xp);
    return (rv);
}

extern void chvt_reinit(void);


#define INVTCK(r, t)                                                                               \
    do {                                                                                           \
        r.tv_usec = ((t) % (1000000 / CLK_TCK)) * (1000000 / CLK_TCK);                             \
        r.tv_sec = (t) / CLK_TCK;                                                                  \
    } while (/* CONSTCOND */ 0)

int
qsh_getrusage(int what, struct rusage *ru)
{
    struct tms tms;
    clock_t u, s;
#ifndef CLK_TCK
    long CLK_TCK;
#endif

    if (/* ru == NULL || */ times(&tms) == (clock_t)-1)
        return (-1);

    switch (what) {
    case RUSAGE_SELF:
        u = tms.tms_utime;
        s = tms.tms_stime;
        break;
    case RUSAGE_CHILDREN:
        u = tms.tms_cutime;
        s = tms.tms_cstime;
        break;
    default:
        errno = EINVAL;
        return (-1);
    }
#ifndef CLK_TCK
#ifdef ENOSYS
    errno = ENOSYS;
#else
    errno = EINVAL;
#endif
    if ((CLK_TCK = sysconf(_SC_CLK_TCK)) == -1L)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_ONEMSG, "sysconf(_SC_CLK_TCK)");
#endif
    INVTCK(ru->ru_utime, u);
    INVTCK(ru->ru_stime, s);
    return (0);
}

/*
 * process the string available via fg (get a char)
 * and fp (put back a char) for backslash escapes,
 * assuming the first call to *fg gets the char di-
 * rectly after the backslash; return the character
 * (0..0xFF), UCS (wc + 0x100), or -1 if no known
 * escape sequence was found
 */
int
unbksl(bool cstyle, int (*fg)(void), void (*fp)(int))
{
    int wc, i, c, fc, n;

    fc = (*fg)();
    switch (fc) {
    case 'a':
        wc = QSH_BEL;
        break;
    case 'b':
        wc = '\b';
        break;
    case 'c':
        if (!cstyle)
            goto unknown_escape;
        c = (*fg)();
        wc = asc2rtt(ord(c) == ORD('?') ? 0x7F : rtt2asc(c) & 0x9F);
        break;
    case 'E':
    case 'e':
        wc = QSH_ESC;
        break;
    case 'f':
        wc = '\f';
        break;
    case 'n':
        wc = '\n';
        break;
    case 'r':
        wc = '\r';
        break;
    case 't':
        wc = '\t';
        break;
    case 'v':
        wc = QSH_VTAB;
        break;
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        if (!cstyle)
            goto unknown_escape;
        /* FALLTHROUGH */
    case '0':
        if (cstyle)
            (*fp)(fc);
        /*
         * look for an octal number with up to three
         * digits, not counting the leading zero;
         * convert it to a raw octet
         */
        wc = 0;
        i = 3;
        while (i--)
            if (ctype((c = (*fg)()), C_OCTAL))
                wc = (wc << 3) + qsh_numdig(c);
            else {
                (*fp)(c);
                break;
            }
        break;
    case 'U':
        i = 8;
        if (/* CONSTCOND */ 0)
            /* FALLTHROUGH */
        case 'u':
            i = 4;
        if (/* CONSTCOND */ 0)
            /* FALLTHROUGH */
        case 'x':
            i = cstyle ? -1 : 2;
        /**
         * x:   look for a hexadecimal number with up to
         *  two (C style: arbitrary) digits; convert
         *  to raw octet (C style: UCS if >0xFF)
         * u/U: look for a hexadecimal number with up to
         *  four (U: eight) digits; convert to UCS
         */
        wc = 0;
        n = 0;
        while (n < i || i == -1) {
            wc <<= 4;
            if (!ctype((c = (*fg)()), C_SEDEC)) {
                wc >>= 4;
                (*fp)(c);
                break;
            }
            if (ctype(c, C_DIGIT))
                wc += qsh_numdig(c);
            else if (ctype(c, C_UPPER))
                wc += qsh_numuc(c) + 10;
            else
                wc += qsh_numlc(c) + 10;
            ++n;
        }
        if (!n)
            goto unknown_escape;
        if ((cstyle && wc > 0xFF) || fc != 'x')
            /* UCS marker */
            wc += 0x100;
        break;
    case '\'':
        if (!cstyle)
            goto unknown_escape;
        wc = '\'';
        break;
    case '\\':
        wc = '\\';
        break;
    default:
    unknown_escape:
        (*fp)(fc);
        return (-1);
    }

    return (wc);
}

