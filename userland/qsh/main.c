/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Shell startup: command-line parsing, environment import, profile/rc
 * sourcing, hand-off to shell().  Also the definition site for all
 * EXTERN-declared globals (`#define EXTERN` is empty here, so each
 * EXTERN line in sh.h becomes a real definition compiled into main.o).
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define EXTERN
#include "sh.h"

__IDSTRING(qsh_cc_h_rcsid, SYSKERN_QSH_CC_H);
__IDSTRING(qsh_intmath_h_rcsid, SYSKERN_QSH_INTMATH_H);
__IDSTRING(sh_h_rcsid, QSH_SH_H_ID);

#ifndef QSHRC_PATH
#define QSHRC_PATH "~/.qshrc"
#endif

static void main_init(int, const char *[], Source **);
void chvt_reinit(void);
static void rndsetup(void);
static void init_environ(void);
static void x_sigwinch(int);

static const char initsubs[] = "${PS2=> }"
                               "${PS3=#? }"
                               "${PS4=+ }"
                               "${SECONDS=0}"
                               "${TMOUT=0}"
                               "${EPOCHREALTIME=}";

static const char *initcoms[] = {
    Ttypeset, Tdr, initvsn, NULL, Ttypeset, Tdx, "HOME", TPATH, TSHELL, NULL, Ttypeset, "-i10",
    "COLUMNS", "LINES", "SECONDS", "TMOUT", NULL, Talias, "integer=\\\\builtin typeset -i",
    "local=\\\\builtin typeset",
    /* not "alias -t --": hash -r needs to work */
    "hash=\\\\builtin alias -t", "type=\\\\builtin whence -v", "autoload=\\\\builtin typeset -fu",
    "functions=\\\\builtin typeset -f", "history=\\\\builtin fc -l",
    "nameref=\\\\builtin typeset -n", "nohup=nohup ", "r=\\\\builtin fc -e -",
    "login=\\\\builtin exec login", NULL,
    /* this is what AT&T ksh seems to track, with the addition of emacs */
    Talias, "-tU", "cat", "cc", "chmod", "cp", "date", "ed", "emacs", "grep", "ls", "make", "mv",
    "pr", "rm", "sed", Tsh, "vi", "who", NULL, NULL};

extern const char Tpipest[];

/* top-level parsing and execution environment */
static struct env env;
struct env *e = &env;

/* getcwd() buffer — also used by qsh_getwd() */
static size_t getwd_bufsz = 448U;
static char *getwd_bufp = NULL;

/* many compile-time assertions */
qCTA_BEG(main_c);

/* require char to be 8 bit long */
qCTA(char_8bit, (CHAR_BIT) == 8 && (((unsigned int)(unsigned char)255U) == 255U) &&
                     (((unsigned int)(unsigned char)256U) == 0U) &&
                     qiTYPE_UBITS(unsigned char) == 8U && qiMASK_BITS(SCHAR_MAX) == 7U);

/* the next assertion is probably not really needed */
qCTA(short_is_2_char, sizeof(short) == 2);
/* the next assertion is probably not really needed */
qCTA(int_is_4_char, sizeof(int) == 4);

qiCTA_TYPE_MBIT(sari, qsh_ari_t);
qiCTA_TYPE_MBIT(uari, qsh_uari_t);
qCTA(basic_int32_smask, qiMASK_CHK(INT32_MAX));
qCTA(basic_int32_umask, qiMASK_CHK(UINT32_MAX));
qCTA(basic_int32_ari, qiTYPE_UMAX(qsh_uari_t) == (UINT32_MAX) &&
                           /* require two’s complement */
                           ((INT32_MIN) + 1 == -(INT32_MAX)));
/* the next assertion is probably not really needed */
qCTA(ari_is_4_char, sizeof(qsh_ari_t) == 4);
/* but this is */
qCTA(ari_has_31_bit, qiMASK_BITS(INT32_MAX) == 31);
/* the next assertion is probably not really needed */
qCTA(uari_is_4_char, sizeof(qsh_uari_t) == 4);
qCTA(uari_is_32_bit, qiTYPE_UBITS(qsh_uari_t) == 32);
/* these are always required */
qCTA(ari_is_signed, !qiTYPE_ISU(qsh_ari_t));
qCTA(uari_is_unsigned, qiTYPE_ISU(qsh_uari_t));
/* we require these to have the precisely same size and assume 2s complement */
qCTA(ari_size_no_matter_of_signedness, sizeof(qsh_ari_t) == sizeof(qsh_uari_t));

/* our formatting routines assume this */
qCTA(ari_fits_in_long, sizeof(qsh_ari_t) <= sizeof(long));

qCTA_END(main_c);
/* end of compile-time asserts */

static void
rndsetup(void)
{
    struct {
        ALLOC_ITEM alloc_INT;
        void *bssptr, *dataptr, *stkptr, *mallocptr;
        qshjmp_buf jbuf;
        struct timeval tv;
    } *bufptr;
    char *cp;

    cp = alloc(sizeof(*bufptr) - sizeof(ALLOC_ITEM), APERM);
    /* clear the allocated space, for valgrind and to avoid UB */
    memset(cp, 0, sizeof(*bufptr) - sizeof(ALLOC_ITEM));
    /* undo what alloc() did to the malloc result address */
    bufptr = (void *)(cp - sizeof(ALLOC_ITEM));
    /* PIE or something similar provides us with deltas here */
    bufptr->bssptr = &rndsetupstate;
    bufptr->dataptr = &e;
    /* ASLR in at least Windows, Linux, some BSDs */
    bufptr->stkptr = &bufptr;
    /* randomised malloc in BSD (and possibly others) */
    bufptr->mallocptr = bufptr;
    /* glibc pointer guard hook (no-op on QRV — kept for stack churn) */
    setjmp(bufptr->jbuf);
    /* introduce variation (cannot use gettimeofday *tzp portably) */
    qsh_TIME(bufptr->tv);

    chvt_rndsetup(bufptr, sizeof(*bufptr));
    afree(cp, APERM);
}

/* pre-initio() */
void
chvt_reinit(void)
{
    qshpid = procpid = getpid();
    qsheuid = geteuid();
    qshpgrp = getpgrp();
    qshppid = getppid();
}

static const char *empty_argv[] = {Tqsh, NULL};

static kby
isuc(const char *cx)
{
    const char *cp;

    if (!cx || !*cx)
        return (0);

    if ((cp = cstrchr(cx, '.')))
        ++cp;
    else
        cp = cx;
    if (!isCh(cp[0], 'U', 'u') || !isCh(cp[1], 'T', 't') || !isCh(cp[2], 'F', 'f'))
        return (0);
    cp += isch(cp[3], '-') ? 4 : 3;
    return (isch(*cp, '8') && (isch(cp[1], '@') || !cp[1]));
}

kby
qshname_islogin(const char **qshbasenamep)
{
    const char *cp;
    size_t o;
    kby rv;

    /* determine the basename (without '-' or path) of the executable */
    cp = qshname;
    o = 0;
    while ((rv = cp[o++])) {
        if (qsh_cdirsep(rv)) {
            cp += o;
            o = 0;
        }
    }
    rv = isch(*cp, '-') || isch(*qshname, '-');
    if (isch(*cp, '-'))
        ++cp;
    if (!*cp)
        cp = empty_argv[0];
    *qshbasenamep = cp;
    return (rv);
}

/* pre-initio() */
static void
main_init(int argc, const char *argv[], Source **sp)
{
    int argi = 1, i;
    Source *s = NULL;
    unsigned char errexit, utf_flag;
    char *cp;
    const char *ccp, **wp;
    struct tbl *vp;
    struct stat s_stdin;

    set_ifs(TC_IFSWS);

    /* do things like getpgrp() et al. */
    chvt_reinit();

    /* make sure argv[] is sane, for weird OSes */
    if (!*argv) {
        argv = empty_argv;
        argc = 1;
    }
    qshname = argv[0];

    /* initialise permanent Area */
    ainit(&aperm);
    /* max. name length: -2147483648 = 11 (+ NUL) */
    vtemp = alloc(qccFAMSZ(struct tbl, name, 12), APERM);
    getwd_bufp = alloc(getwd_bufsz + 1U, APERM);
    getwd_bufp[getwd_bufsz] = '\0';

    /* set up base environment */
    env.type = E_NONE;
    ainit(&env.area);
    /* set up global l->vars and l->funs */
    newblock();

    /* Do this first so output routines (eg. kwarnf, shellf) can work */
    initio();

    /* check qshname: leading dash, determine basename */
    Flag(FLOGIN) = qshname_islogin(&ccp);

    /*
     * Turn on nohup by default. (AT&T ksh does not have a nohup
     * option - it always sends the hup).
     */
    Flag(FNOHUP) = 1;

    /*
     * Turn on brace expansion by default. AT&T qshs that have
     * alternation always have it on.
     */
    Flag(FBRACEEXPAND) = 1;

    /*
     * Turn on "set -x" inheritance by default.
     */
    Flag(FXTRACEREC) = 1;

    /* define built-in commands and see if we were called as one */
    ktinit(APERM, &builtins,
           /* currently up to 50 builtins: 75% of 128 = 2^7 */
           7);
    for (i = 0; qshbuiltins[i].name != NULL; ++i) {
        const char *builtin_name;

        builtin_name = builtin(qshbuiltins[i].name, qshbuiltins[i].func);
        if (!strcmp(ccp, builtin_name)) {
            /* canonicalise argv[0] */
            ccp = builtin_name;
            as_builtin = true;
        }
    }

    if (!as_builtin) {
        /* check for -T option early */
        argi = parse_args(argv, OF_FIRSTTIME, NULL);
        if (argi < 0)
            unwind(LERROR);
    }

    initvar();

    inittraps();

    coproc_init();

    /* set up variable and command dictionaries */
    ktinit(APERM, &taliases, 0);
    ktinit(APERM, &aliases, 0);

    /* define shell keywords */
    initkeywords();

    init_histvec();

    /* initialise tty size before importing environment */
    change_winsz();

    def_path =
        QSH_UNIXROOT "/bin" QSH_PATHSEPS QSH_UNIXROOT "/usr/bin" QSH_PATHSEPS QSH_UNIXROOT
                      "/sbin" QSH_PATHSEPS QSH_UNIXROOT "/usr/sbin";

    /*
     * Set PATH to def_path (will set the path global variable).
     * Import of environment below will probably change this setting;
     * the EXPORT flag is only added via initcoms for this to work.
     */
    vp = global(TPATH);
    /* setstr can't fail here */
    setstr(vp, def_path, QSH_RETURN_ERROR);

    /* QRV: line editor is hardcoded — no FEMACS/FVI toggle.
       The editor is always on for ttys; non-tty input bypasses it. */

    /* import environment */
    init_environ();

    /* for security */
    typeset(TinitIFS, 0, 0, 0, 0);

    /* assign default shell variable values */
    typeset("PATHSEP=" QSH_PATHSEPS, 0, 0, 0, 0);
    substitute(initsubs, 0);

    /* Figure out the current working directory and set $PWD */
    vp = global(TPWD);
    cp = str_val(vp);
    /* Try to use existing $PWD if it is valid */
    set_current_wd((qsh_abspath(cp) && test_eval(NULL, TO_FILEQ, cp, Tdot, true)) ? cp : NULL);
    if (current_wd[0])
        simplify_path(current_wd);
    /* Only set pwd if we know where we are or if it had a bogus value */
    if (current_wd[0] || *cp)
        /* setstr can't fail here */
        setstr(vp, current_wd, QSH_RETURN_ERROR);

    for (wp = initcoms; *wp != NULL; wp++) {
        c_builtin(wp);
        while (*wp != NULL)
            wp++;
    }
    setint_n(global("OPTIND"), 1, 10);

    qshuid = getuid();
    qshgid = getgid();
    qshegid = getegid();
    rndsetup();

    /* QRV default: bash-style "[\w]\$ ".  Set unconditionally so an
       inherited (and exported) PS1 from a parent shell can't pin the
       child to a stale "#"/"$".  set_prompt() expands \w → cwd and
       \$ → '#' (root) or '$' (non-root) at every prompt print. */
    safe_prompt = "[$PWD]# ";   /* literal fallback for prompt errors */
    vp = global("PS1");
    setstr(vp, "[\\w]\\$ ", QSH_RETURN_ERROR);
    /* ensure several variables will be (unsigned) integers */
    setint_n((vp = global("BASHPID")), 0, 10);
    vp->flag |= INT_U;
    setint_n((vp = global("PGRP")), (qsh_uari_t)qshpgrp, 10);
    vp->flag |= INT_U;
    setint_n((vp = global("PPID")), (qsh_uari_t)qshppid, 10);
    vp->flag |= INT_U;
    setint_n((vp = global("USER_ID")), (qsh_uari_t)qsheuid, 10);
    vp->flag |= INT_U;
    setint_n((vp = global("QSHUID")), (qsh_uari_t)qshuid, 10);
    vp->flag |= INT_U;
    setint_n((vp = global("QSHEGID")), (qsh_uari_t)qshegid, 10);
    vp->flag |= INT_U;
    setint_n((vp = global("QSHGID")), (qsh_uari_t)qshgid, 10);
    vp->flag |= INT_U;
    /* this is needed globally */
    setint_n((vp_pipest = global(Tpipest)), 0, 10);
    /* unsigned integer, but avoid rndset call: done farther below */
    vp = global("RANDOM");
    vp->flag &= ~SPECIAL;
    setint_n(vp, 0, 10);
    vp->flag |= SPECIAL | INT_U;

    /* Set this before parsing arguments */
    Flag(FPRIVILEGED) = (qshuid != qsheuid || qshgid != qshegid) ? 2 : 0;

    /* this to note if utf-8 mode is set on command line (see below) */
    UTFMODE = 2;

    if (!as_builtin) {
        argi = parse_args(argv, OF_CMDLINE, NULL);
        if (argi < 0)
            unwind(LERROR);
    }

    /*XXX drop this and the entire cases below */
    /* process this later only, default to off (hysterical raisins) */
    utf_flag = UTFMODE;
    UTFMODE = 0;

    if (as_builtin) {
        /* auto-detect from environment variables, always */
        utf_flag = 3;
    } else if (Flag(FCOMMAND)) {
        s = pushs(SSTRINGCMDLINE, ATEMP);
        if (!(s->start = s->str = argv[argi++]))
            kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, Tdc, Treq_arg);
        while (*s->str) {
            if (ctype(*s->str, C_QUOTE))
                break;
            s->str++;
        }
        if (!*s->str)
            s->flags |= SF_MAYEXEC;
        s->str = s->start;
        if (argv[argi])
            qshname = argv[argi++];
    } else if (argi < argc && !Flag(FSTDIN)) {
        s = pushs(SFILE, ATEMP);
        s->file = argv[argi++];
        s->u.shf = shf_open(s->file, O_RDONLY | O_MAYEXEC, 0, SHF_MAPHI | SHF_CLEXEC);
        if (s->u.shf == NULL) {
            shl_stdout_ok = false;
            kwarnf(KWF_ERR(127) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG, s->file);
            unwind(LERROR);
        }
        qshname = s->file;
    } else {
        Flag(FSTDIN) = 1;
        s = pushs(SSTDIN, ATEMP);
        s->file = "<stdin>";
        s->u.shf = shf_fdopen(0, SHF_RD | can_seek(0), NULL);
        if (isatty(0) && isatty(2)) {
            Flag(FTALKING) = Flag(FTALKING_I) = 1;
            /* The following only if isatty(0) */
            s->flags |= SF_TTY;
            s->u.shf->flags |= SHF_INTERRUPT;
            s->file = NULL;
        }
    }

    /* this bizarreness is mandated by POSIX */
    if (Flag(FTALKING) && fstat(0, &s_stdin) >= 0 &&
        (S_ISCHR(s_stdin.st_mode) || S_ISFIFO(s_stdin.st_mode)))
        reset_nonblock(0);

    /* initialise job control */
    j_init();
    /* do this after j_init() which calls tty_init_state() */
    if (Flag(FTALKING)) {
        if (utf_flag == 2) {
            /* auto-detect from environment */
            utf_flag = 4;
        }
        x_init();
    }

    sigtraps[SIGWINCH].flags |= TF_SHELL_USES;
    setsig(&sigtraps[SIGWINCH], x_sigwinch, SS_RESTORE_ORIG | SS_FORCE | SS_SHTRAP);

    /*
     * allocate a new array because otherwise, when we modify
     * it in-place, ps(1) output changes; the meaning of argc
     * here is slightly different as it excludes qshname, and
     * we add a trailing NULL sentinel as well
     */
    e->loc->argc = argc - argi;
    e->loc->argv = alloc2(e->loc->argc + 2, sizeof(char *), APERM);
    memcpy(&e->loc->argv[1], &argv[argi], e->loc->argc * sizeof(char *));
    e->loc->argv[e->loc->argc + 1] = NULL;
    if (as_builtin)
        e->loc->argv[0] = ccp;
    else {
        e->loc->argv[0] = qshname;
        getopts_reset(1);
    }

    /* divine the initial state of the utf8-mode Flag */
    ccp = null;
    switch (utf_flag) {
    /* auto-detect from LC_ALL / LC_CTYPE / LANG (already in environ) */
    case 4:
    case 3:
        if (ccp == null)
            ccp = str_val(global("LC_ALL"));
        if (ccp == null)
            ccp = str_val(global("LC_CTYPE"));
        if (ccp == null)
            ccp = str_val(global("LANG"));
        UTFMODE = isuc(ccp);
        break;

    /* not set on command line, not FTALKING — leave UTFMODE off */
    case 2:
    /* unknown values */
    default:
        utf_flag = 0;
        /* FALLTHROUGH */

    /* known values */
    case 1:
    case 0:
        UTFMODE = utf_flag;
        break;
    }

    /* Disable during .profile/ENV reading */
    errexit = Flag(FERREXIT);
    Flag(FERREXIT) = 0;

    /* save flags for "set +o" handling */
    memcpy(baseline_flags, shell_flags, sizeof(shell_flags));
    /* disable these because they have special handling */
    baseline_flags[(int)FPOSIX] = 0;
    baseline_flags[(int)FSH] = 0;
    /* ensure these always show up setting, for FPOSIX/FSH */
    baseline_flags[(int)FBRACEEXPAND] = 0;
    baseline_flags[(int)FUNNYCODE] = 0;
    /* mark as initialised */
    baseline_flags[(int)FNFLAGS] = 1;
    rndpush(shell_flags, sizeof(shell_flags));
    rndset(hash(/*=current_wd*/ cp) ^ hash(argv[0]));
    if (as_builtin)
        goto skip_startup_files;

    /*
     * Do this before profile/$ENV so that if it causes problems in them,
     * user will know why things broke.
     */
    if (!current_wd[0] && Flag(FTALKING))
        kwarnf(KWF_PREFIX | KWF_ONEMSG | KWF_NOERRNO, "can't determine current directory");

    if (Flag(FLOGIN))
        include(QSH_SYSTEM_PROFILE, NULL, true);
    if (Flag(FPRIVILEGED)) {
        include(QSH_SUID_PROFILE, NULL, true);
        /* note whether -p was enabled during startup */
        if (Flag(FPRIVILEGED) == 1)
            /* allow set -p to setuid() later */
            Flag(FPRIVILEGED) = 3;
        else
            /* turn off -p if not set explicitly */
            change_flag(FPRIVILEGED, OF_INTERNAL, false);
        /* track shell-imposed changes */
        baseline_flags[(int)FPRIVILEGED] = Flag(FPRIVILEGED);
    } else {
        if (Flag(FLOGIN))
            include(substitute("$HOME/.profile", 0), NULL, true);
        if (Flag(FTALKING)) {
            cp = substitute("${ENV:-" QSHRC_PATH "}", DOTILDE);
            if (cp[0] != '\0')
                include(cp, NULL, true);
        }
    }
    Flag(FERREXIT) = errexit;

    if (Flag(FTALKING) && s)
        hist_init(s);
    else {
        /* set after ENV */
    skip_startup_files:
        Flag(FTRACKALL) = 1;
        /* track shell-imposed change (might lower surprise) */
        baseline_flags[(int)FTRACKALL] = 1;
    }

    alarm_init();

    *sp = s;
}

/* this indirection barrier reduces stack usage during normal operation */

int
main(int argc, const char *argv[])
{
    int rv;
    Source *s;

    /* v0.6.4 trace: confirm we got past crt0 + libqsoe init. */
    extern int printf(const char *, ...);
    printf("[qsh] main entered, argc=%d argv[0]=%s\n",
           argc, argc > 0 ? argv[0] : "(none)");

    main_init(argc, argv, &s);
    if (as_builtin) {
        rv = c_builtin(e->loc->argv) & 0xFF;
        exstat = rv;
        unwind(LEXIT);
        /* NOTREACHED */
    } else {
        rv = shell(s, 0) & 0xFF;
        /* NOTREACHED */
    }
    return (rv);
}

static void
x_sigwinch(int sig QSH_A_UNUSED)
{
    /* this runs inside interrupt context, with errno saved */

    got_winch = 1;
}

extern char **environ;

static void
init_environ(void)
{
    const char **wp;

    if (environ == NULL)
        return;

    wp = (const char **)environ;
    while (*wp != NULL) {
        rndpush(*wp, strlen(*wp));
        typeset(*wp, IMPORT | EXPORT, 0, 0, 0);
        ++wp;
    }
}

const char *
qsh_getwd(void)
{
redo:
    if (getcwd(getwd_bufp, getwd_bufsz)) {
        if (qsh_abspath(getwd_bufp))
            return (getwd_bufp);
        errno = EACCES;
        return (NULL);
    }
    if (errno == ERANGE) {
        if (notoktomul(getwd_bufsz, 2U)) {
            errno = ENAMETOOLONG;
            return (NULL);
        }
        getwd_bufsz <<= 1;
        getwd_bufp = aresize(getwd_bufp, getwd_bufsz + 1U, APERM);
        getwd_bufp[getwd_bufsz] = '\0';
        goto redo;
    }
    return (NULL);
}
