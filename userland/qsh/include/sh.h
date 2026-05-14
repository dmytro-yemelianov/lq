/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * Adaptation for QRV: Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Master header for the QRV shell.  Rebuilt from scratch — the
 * upstream sh.h was a 3000-line aggregate of 30 years of platform
 * accretion.  Each section here exists because some .c file
 * actually needs it; nothing is here just because mksh had it.
 */

#ifndef _QRV_SH_H
#define _QRV_SH_H

/* qsh build configuration — formerly fed via `-include qsh_config.h`
 * from the upstream mksh Makefile.  Pulling it in here ensures every
 * .c that includes sh.h sees HAVE_* / QSH_* feature gates before the
 * system headers below check them. */
#include "qsh_config.h"

/* -----------------------------------------------------------------
 * 1. System headers — the union of what the .c files need.
 * ----------------------------------------------------------------- */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/times.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <paths.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <spawn.h>

/* -----------------------------------------------------------------
 * 2. qsh integer / compiler toolkit.
 *    Provides SHIKATANAI, the qi* arithmetic helpers, qnil, etc.
 *    qccABEND is defined here first so the headers don't fall back
 *    to abort(); we route to kerrf0 instead so users get a real
 *    "internal error" message.  See section 8 below for kerrf0.
 * ----------------------------------------------------------------- */

#define QSH_INT_H_WANT_PTR_IN_SIZET 1
#define QSH_INT_H_WANT_SIZET_IN_LONG 1
#define QSH_INT_H_WANT_INT32 1
#define QSH_INT_H_WANT_SAFEC 1
/* forward — defined in section 8 once KWF_* are in scope */
extern void kerrf0(unsigned int, const char *, ...);
#define qccABEND(reason)                                                                  \
    kerrf0(0x000200U | 0x0000FFU | 0x000400U | 0x004000U | 0x010000U, (reason))
#include "qsh_cc.h"
#include "qsh_intmath.h"

/* -----------------------------------------------------------------
 * 3. Compiler attributes (we always have gcc on RV64).
 * ----------------------------------------------------------------- */

#define QSH_A_BOUNDED(x, y, z)         /* nothing — gcc bounded ext absent */
#define QSH_A_FORMAT(x, y, z)          __attribute__((__format__(x, y, z)))
#define QSH_A_NORETURN                 __attribute__((__noreturn__))
#define QSH_A_UNUSED                   __attribute__((__unused__))
#define QSH_A_USED                     __attribute__((__used__))
#define QSH_A_PURE                     __attribute__((__pure__))

/* identifier-string macro — mksh's own.  No-op: we don't emit
   RCS strings.  The FreeBSD __FBSDID/__RCSID/__SCCSID/__COPYRIGHT
   family that used to live in <sys/cdefs.h> has been deleted from
   QRV; if a newly imported file ever reaches for one, fix the
   import — don't reintroduce no-op shims. */
#define __IDSTRING(prefix, string)      /* nothing */

#define QSH_VERSION                    "QSOE-v0.6.3 (based on mksh R59)"

/* -----------------------------------------------------------------
 * 4. Integer-rank typedefs.
 *
 *    Booleans use C99 stdbool.h (true/false/bool) — the upstream
 *    German-named bool/true/false/isWahr typedef + macros have been
 *    replaced throughout the QRV port.
 * ----------------------------------------------------------------- */

typedef unsigned char           kby;        /* one byte */
typedef unsigned int            kui;        /* int-rank, used for wide-byte / EOF */
typedef unsigned int            k32;        /* exactly 32-bit (CFLAGS guarantee) */
typedef unsigned long           kul;        /* long-rank arithmetic */
typedef signed long             ksl;        /* long-rank signed arithmetic */

#define KBY(c)                  ((kby)(KUI(c) & 0xFFU))
#define KBI(c)                  ((kui)(KUI(c) & 0xFFU))
#define KUI(u)                  ((kui)(u))

#define K32_HM                  0x7FFFFFFFUL
#define K32_FM                  0xFFFFFFFFUL
#define KUL_FM                  ULONG_MAX
#define KUL_HM                  LONG_MAX

/* shell arithmetic — exactly 32-bit, signed and unsigned */
typedef int32_t                 qsh_ari_t;
typedef uint32_t                qsh_uari_t;
#define KUA_HM                  K32_HM
#define KUA_FM                  K32_FM

/* qi* huge type (used by some hashing code) */
typedef kul                     kuH;
typedef ksl                     ksH;
#define KUH_FM                  ULONG_MAX
#define KUH_HM                  LONG_MAX

/* -----------------------------------------------------------------
 * 5. Common one-liner helpers.
 * ----------------------------------------------------------------- */

#define BIT(i)                  (1U << (i))
#define NELEM(a)                (sizeof(a) / sizeof((a)[0]))
#define SC(s)                   (s), (sizeof(s) / sizeof(s[0]) - 1U)
#define SZ(s)                   (s), strlen(s)
#define IS(v, f, t)             (((v) & (f)) == (t))
#define HAS(v, f)               (((v) & (f)) == (f))
#define strnul(s)               ((s) + strlen((const void *)(s)))

/* allocator-backed strdup variants — fail by calling kerrf */
#define strdupx(d, s, ap)                                                                  \
    do {                                                                                   \
        const char *strdup_src = (const void *)(s);                                        \
        char       *strdup_dst = NULL;                                                     \
        if (strdup_src != NULL) {                                                          \
            size_t strdup_len = strlen(strdup_src) + 1U;                                   \
            strdup_dst = alloc(strdup_len, (ap));                                          \
            memcpy(strdup_dst, strdup_src, strdup_len);                                    \
        }                                                                                  \
        (d) = strdup_dst;                                                                  \
    } while (/* CONSTCOND */ 0)
#define strndupx(d, s, n, ap)                                                              \
    do {                                                                                   \
        const char *strdup_src = (const void *)(s);                                        \
        char       *strdup_dst = NULL;                                                     \
        if (strdup_src != NULL) {                                                          \
            size_t strndup_len = (n);                                                      \
            strdup_dst = alloc(strndup_len + 1U, (ap));                                    \
            memcpy(strdup_dst, strdup_src, strndup_len);                                   \
            strdup_dst[strndup_len] = '\0';                                                \
        }                                                                                  \
        (d) = strdup_dst;                                                                  \
    } while (/* CONSTCOND */ 0)

/* TAB and a few other useful character constants */
#define CTRL_I                  0x09U

/*
 * const-launder unions: cast away const without UB-by-the-letter.
 * Used by tree.c and friends when an mksh API takes char ** but
 * the caller has const char **.
 */
union qsh_cchack {
    char       *rw;
    const char *ro;
};
union qsh_ccphack {
    char       **rw;
    const char **ro;
};

/* helpers above the standard <string.h> functions */
#define ucstrchr(s, c)          strchr((s), (c))
#define cstrchr(s, c)           ((const char *)strchr((s), (c)))
#define vstrchr(s, c)           (cstrchr((s), (c)) != NULL)
#define ucstrstr(s, c)          strstr((s), (c))
#define cstrstr(s, c)           ((const char *)strstr((s), (c)))
#define vstrstr(b, l)           (cstrstr((b), (l)) != NULL)

#define qsh_TIME(tv)           gettimeofday(&(tv), NULL)

/* additional alloc-backed string macros (strdupx is above).
   strnbdupx: like strndupx but uses a static char-array buffer `b`
   when the source fits, only allocing if it doesn't. */
#define strnbdupx(d, s, n, ap, b)                                                          \
    do {                                                                                   \
        const char *strdup_src = (const void *)(s);                                        \
        char       *strdup_dst = NULL;                                                     \
        if (strdup_src != NULL) {                                                          \
            size_t strndup_len = (n);                                                      \
            strdup_dst = strndup_len < sizeof(b)                                           \
                       ? (b) : alloc(strndup_len + 1U, (ap));                              \
            memcpy(strdup_dst, strdup_src, strndup_len);                                   \
            strdup_dst[strndup_len] = '\0';                                                \
        }                                                                                  \
        (d) = strdup_dst;                                                                  \
    } while (/* CONSTCOND */ 0)
#define strdup2x(d, s1, s2)                                                                \
    do {                                                                                   \
        const char *strdup_src = (const void *)(s1);                                       \
        const char *strdup_app = (const void *)(s2);                                       \
        size_t strndup_len = strlen(strdup_src);                                           \
        size_t strndup_ln2 = strlen(strdup_app) + 1U;                                      \
        char  *strdup_dst  = alloc1(strndup_len, strndup_ln2, ATEMP);                      \
        memcpy(strdup_dst, strdup_src, strndup_len);                                       \
        memcpy(strdup_dst + strndup_len, strdup_app, strndup_ln2);                         \
        (d) = strdup_dst;                                                                  \
    } while (/* CONSTCOND */ 0)

#define ord(c)                  KBI(c)      /* char -> unsigned int */

/* fd ceiling for shf I/O — anything above this is "shf-owned" */
#define FDBASE                  10

/* binary-mode open: QRV has no text mode, so this is plain open() */
#define binopen2(path, flags)         open((path), (flags), 0)
#define binopen3(path, flags, mode)   open((path), (flags), (mode))

/* number of pre-reserved file descriptors per process (0..NUFILE-1) */
#define NUFILE                  10

/* MAGIC byte used by the expander to mark special positions in
   words.  Followed by the literal byte being escaped. */
#define QSH_BEL                 7
#define MAGIC                   QSH_BEL
#define ISMAGIC(c)              (ord(c) == ORD(MAGIC))

/* eval / substitute() flag bits */
#define ONEWORD                 BIT(1)  /* substitute(): single word */

/* path separator on POSIX (not on the dead OS/2 / DOS paths) */
#define QSH_PATHSEPC           ':'
#define QSH_PATHSEPS           ":"

/* lstat() — straight pass-through on QRV (we have it) */
#define qsh_lstat              lstat
/* const-correct strerror — same string, but with the right type */
#define cstrerror(errnum)       ((const char *)strerror(errnum))

/* O_MAYEXEC — Linux extension, not on QRV; harmless 0 */
#define O_MAYEXEC               0

/* exit-status encoding for non-zero signal exits */
#define qsh_sigmask(sig)        (((sig) < 1 || (sig) > 127) ? 255 : 128 + (sig))

/* HEREDOC bit — used by the syntax tree state machine */
#define HEREDOC                 BIT(6)

/* control-character constants used by tree-printing (uprntc, etc.) */
#define QSH_ESC                 033
#define QSH_VTAB                11

/* Signal-message stubs.  Upstream had a complex setting for systems
   that knew their own sig-message strings; QRV picks them up from
   strsignal() via histrap.c, so these are no-op placeholders. */
#define qsh_sigmess(nr)         NULL
#define qsh_sigmessf(mess)      (!(mess) || !*(mess))

/* SMALLP — used in #ifndef QSH_SMALL function-prototype tweaks.
   Since QRV doesn't build the SMALL variant, this expands to nothing
   (the 1st arg is consumed; the trailing comma+arg becomes empty). */
#define SMALLP(x)               , x

/* concat path1 "/" path2 into newly alloc'd buffer */
#define strpathx(d, s1, s2, cond)                                                          \
    do {                                                                                   \
        const char *strdup_src = (const void *)(s1);                                       \
        const char *strdup_app = (const void *)(s2);                                       \
        size_t strndup_len = strlen(strdup_src) + 1U;                                      \
        size_t strndup_ln2 = ((cond) || *strdup_app)                                       \
                              ? strlen(strdup_app) + 1U : 0;                               \
        char  *strdup_dst  = alloc1(strndup_len, strndup_ln2, ATEMP);                      \
        memcpy(strdup_dst, strdup_src, strndup_len);                                       \
        if (strndup_ln2) {                                                                 \
            strdup_dst[strndup_len - 1U] = '/';                                            \
            memcpy(strdup_dst + strndup_len, strdup_app, strndup_ln2);                     \
        }                                                                                  \
        (d) = strdup_dst;                                                                  \
    } while (/* CONSTCOND */ 0)

/* prompt index */
#define PS1                     0       /* command */
#define PS2                     1       /* command continuation */
#define PS4                     3       /* xtrace prefix */

/* maximum number of << redirections in one line */
#define HERES                   10

/* history-file write modes */
#define HIST_STORE              3

/* version string + system-profile paths.  initvsn itself is
   declared after the EXTERN/E_INIT switch comes into scope below. */
#define QSH_DEFAULT_PROFILEDIR "/etc"
#define QSH_SYSTEM_PROFILE     QSH_DEFAULT_PROFILEDIR "/profile"
#define QSH_SUID_PROFILE       QSH_DEFAULT_PROFILEDIR "/suid_profile"
/* Upstream OS/2 prefix that's pasted in front of "/bin" etc. — empty
   on every POSIX-y system, including QRV. */
#define QSH_UNIXROOT           ""

/* set the VDISABLE state of a c_cc[] slot */
#define QSH_DOVDIS(x)           (x) = _POSIX_VDISABLE

/* fixed-string memcpy — destination already provisioned */
#define memstr(d, s)            memcpy((d), (s), sizeof(s))

/* highest file descriptor number we'll allow in the savedfd table */
#define FDMAXNUM                127

/* histsave() write modes */
#define HIST_FLUSH              0
#define HIST_QUEUE              1
#define HIST_APPEND             2

/* signal-state save/restore wrapper (uses libc sigaction + struct
   sigaction directly, since QRV's signal mask is process-wide and
   we don't need a custom layout). */
typedef struct sigaction qsh_sigsaved;
#define qsh_sighandler(saved)   ((saved).sa_handler)
void  qsh_sigset(int, sig_t, qsh_sigsaved *);

/* overflow-safe arithmetic primitives */
#define notok2add(max, val, c)  ((val) > ((max) - (c)))
#define notok2mul(max, val, c)  (((val) != 0) && ((c) != 0) && (((max) / (c)) < (val)))
#define notoktoadd(val, cnst)   notok2add(qiSIZE_MAX, (val), (cnst))
#define notoktomul(val, cnst)   notok2mul(qiSIZE_MAX, (val), (cnst))
#define checkoktoadd(val, cnst)                                                            \
    do {                                                                                   \
        if (notoktoadd((val), (cnst)))                                                     \
            kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO,                             \
                   Tintovfl, (size_t)(val), '+', (size_t)(cnst));                          \
    } while (/* CONSTCOND */ 0)

/* -----------------------------------------------------------------
 * 6. EXTERN / E_INIT — declare-or-define switch.
 *    main.c does `#define EXTERN` before including this file, which
 *    flips the table from declarations to definitions.  Must come
 *    before any EXTERN-using line below (and before msgs.h).
 * ----------------------------------------------------------------- */

#ifdef EXTERN
#define E_INIT(i)               = i
#else
#define E_INIT(i)               /* nothing */
#define EXTERN                  extern
#define EXTERN_DEFINED
#endif

/* -----------------------------------------------------------------
 * 7. Allocator: mksh's per-area linked-list allocator.
 *    See lalloc.c for the implementation.
 * ----------------------------------------------------------------- */

struct lalloc_common {
    struct lalloc_common       *next;
};

#define ALLOC_ITEM              struct lalloc_common
#define ALLOC_OVERHEAD          (sizeof(ALLOC_ITEM))

typedef struct lalloc_common    Area;

EXTERN Area aperm;                          /* permanent object space */
#define APERM                   (&aperm)
/* ATEMP refers to the current env's temporary area; defined in env.h
   once struct env is in scope. */

/*
 * Backend the per-area allocator is built on.  We use plain malloc
 * directly — no fortify/underrun-catch pages here.
 */
#define malloc_osi(sz)          malloc(sz)
#define realloc_osi(p, sz)      realloc((p), (sz))
#define free_osimalloc(p)       free(p)

void   ainit(Area *);
void  *aresize(void *, size_t, Area *);
void  *aresize1(void *, size_t, size_t, Area *);
void  *aresize2(void *, size_t, size_t, Area *);
#define alloc(n, ap)            aresize(NULL, (n), (ap))
#define alloc1(a, b, ap)        aresize1(NULL, (a), (b), (ap))
#define alloc2(a, b, ap)        aresize2(NULL, (a), (b), (ap))
void   afree(void *, Area *);
void   afreeall(Area *);

/* -----------------------------------------------------------------
 * 8. Warning / error reporting (kerrf, kwarnf).
 *    Implemented in misc.c.  Rich flag set lets callers control
 *    exit status, error prefix, message format.
 * ----------------------------------------------------------------- */

#define KWF_EXSTAT              0x0000FFU   /* mask: exit status */
#define KWF_VERRNO              0x000100U   /* int vararg: use ipv errno */
#define KWF_INTERNAL            0x000200U   /* internal {error,warning} */
#define KWF_WARNING             0x000000U   /* (default) warning */
#define KWF_ERROR               0x000400U   /* error + consequences */
#define KWF_PREFIX              0x000800U   /* run error_prefix() */
#define KWF_FILELINE            0x001000U   /* error_prefix arg = true */
#define KWF_BUILTIN             0x002000U   /* possibly show builtin_argv0 */
#define KWF_MSGMASK             0x00C000U   /* mask: message style */
#define KWF_MSGFMT              0x000000U   /* (default) printf-style */
#define KWF_ONEMSG              0x004000U   /* single string */
#define KWF_TWOMSG              0x008000U   /* two strings, colonised */
#define KWF_THREEMSG            0x00C000U   /* three strings, colonised */
#define KWF_NOERRNO             0x010000U   /* omit strerror */
#define KWF_BIUNWIND            0x020000U   /* tail-call bi_unwind(0) */
#define KWF_ERR(n)              ((((unsigned int)(n)) & KWF_EXSTAT) | KWF_ERROR)
#define KWF_BIERR               (KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | \
                                 KWF_BUILTIN | KWF_BIUNWIND)

void   kwarnf0(unsigned int, const char *, ...) QSH_A_FORMAT(__printf__, 2, 3);
void   kerrf0(unsigned int, const char *, ...)  QSH_A_NORETURN
                                                QSH_A_FORMAT(__printf__, 2, 3);
/* kerrf: variadic printf-style error.  First arg is KWF_*. */
void   kerrf(unsigned int, ...)                 QSH_A_NORETURN;
/* read() that retries on EINTR */
ssize_t blocking_read(int, char *, size_t)      QSH_A_BOUNDED(__buffer__, 2, 3);
/* parse "+N" / "-N" / "N" prefixes */
int    getpn(const char **, int *);
int    getpnh(const char **, qiHUGE_U *);
/* Forward — full decl after shf.h is included below */
struct shf;
void   print_value_quoted(struct shf *, const char *);

/* -----------------------------------------------------------------
 * 9. Misc one-globals + the shell-options table.
 * ----------------------------------------------------------------- */

EXTERN bool initio_done;                    /* true once shf I/O is up */
EXTERN char ifs0;                           /* first byte of $IFS */

/* Shell exit-status / pid globals */
EXTERN pid_t procpid;                       /* PID of running process */
EXTERN int   exstat;                        /* exit status */
EXTERN int   subst_exstat;                  /* exit status of last $(..) */
EXTERN short trap_exstat;                   /* exit status before a trap */
EXTERN kby   trap_nested;                   /* running nested traps */
EXTERN bool  as_builtin;                    /* direct builtin call */

/* Shell identity — real / effective uid+gid + pid+pgrp+ppid.  Bundled
   into one struct so init can rndsetup() over its bytes in one go. */
EXTERN struct {
    uid_t qshuid_v;
    uid_t qsheuid_v;
    gid_t qshgid_v;
    gid_t qshegid_v;
    pid_t qshpgrp_v;
    pid_t qshppid_v;
    pid_t qshpid_v;
} rndsetupstate;
#define qshpid    rndsetupstate.qshpid_v
#define qshpgrp   rndsetupstate.qshpgrp_v
#define qshuid    rndsetupstate.qshuid_v
#define qsheuid   rndsetupstate.qsheuid_v
#define qshgid    rndsetupstate.qshgid_v
#define qshegid   rndsetupstate.qshegid_v
#define qshppid   rndsetupstate.qshppid_v

/* option-source flags for change_flag() */
#define OF_CMDLINE   0x01U
#define OF_SET       0x02U
#define OF_INTERNAL  0x04U
#define OF_FIRSTTIME 0x08U
#define OF_ANY       (OF_CMDLINE | OF_SET | OF_INTERNAL)

/* null value for unset variables */
#define null         (&null_string[2])
extern char null_string[4];

/*
 * Shell options.  The enum sh_flag and its FNFLAGS sentinel are
 * generated from gen/sh_flags.gen; shell_flags[] is the live byte
 * array, baseline_flags[] holds the values the shell started with.
 * Flag(FXTRACE), Flag(FERREXIT), Flag(FUNNYCODE) are the typical
 * call sites.
 */
enum sh_flag {
#define SHFLAGS_ENUMS
#include "sh_flags.gen"
    FNFLAGS                                 /* count of flags */
};

EXTERN kby shell_flags[FNFLAGS];
EXTERN kby baseline_flags[FNFLAGS + 1];     /* +1: room for sentinel */

#define Flag(f)         (shell_flags[(int)(f)])
#define UTFMODE         Flag(FUNNYCODE)

/* utf-8 helpers — implemented in misc.c */
int     utf_widthadj(const char *, const char **);
size_t  utf_mbswidth(const char *);

/* -----------------------------------------------------------------
 * Argument parsing for builtins and the `getopts` builtin.
 * ----------------------------------------------------------------- */

/* Getopt.flags */
#define GF_ERROR     BIT(0)     /* KWF_BIERR if there is an error */
#define GF_PLUSOPT   BIT(1)     /* allow +c as an option */
#define GF_NONAME    BIT(2)     /* don't print argv[0] in errors */

/* Getopt.info */
#define GI_MINUS     BIT(0)     /* an option started with -... */
#define GI_PLUS      BIT(1)     /* an option started with +... */
#define GI_MINUSMINUS BIT(2)    /* arguments terminated with -- */

/* Some libcs declare these globals; suppress so our table-typed
   versions don't collide. */
#undef optarg
#undef optind

typedef struct {
    const char  *optarg;
    int          optind;
    int          uoptind;       /* what user sees in $OPTIND */
    int          flags;
    int          info;
    unsigned     p;             /* index into argv[optind - 1] */
    char         buf[2];        /* bad-option OPTARG value */
} Getopt;

EXTERN Getopt builtin_opt;
EXTERN Getopt user_opt;

/* -----------------------------------------------------------------
 * Expandable strings and pointer-vector — small allocator gadgets
 * used pervasively for word expansion, here-doc bodies, argv
 * accumulation.  All of these allocate from an Area.
 * ----------------------------------------------------------------- */

#define X_EXTRA  20             /* margin past the end */
#define X_WASTE 255             /* must be 2^n - 1 */

typedef struct XString {
    char  *beg;                 /* start of string */
    size_t len;                 /* allocation length minus margin */
    char  *end;                 /* end of buffer */
    Area  *areap;
} XString;

#define XinitN(xs, length, area)                                                           \
    do {                                                                                   \
        (xs).len   = (length);                                                             \
        (xs).areap = (area);                                                               \
        (xs).beg   = alloc((xs).len + X_EXTRA, (xs).areap);                                \
        (xs).end   = (xs).beg + (xs).len;                                                  \
    } while (/* CONSTCOND */ 0)

#define Xinit(xs, xp, length, area)                                                        \
    do {                                                                                   \
        XinitN((xs), (length), (area));                                                    \
        (xp) = (xs).beg;                                                                   \
    } while (/* CONSTCOND */ 0)

#define Xput(xs, xp, c)         (*xp++ = (c))
#define XcheckN(xs, xp, n)                                                                 \
    do {                                                                                   \
        ssize_t XcheckNi = (size_t)((xp) - (xs).beg) + (n) - (xs).len;                     \
        if (XcheckNi > 0)                                                                  \
            (xp) = Xcheck_grow(&(xs), (xp), (size_t)XcheckNi);                             \
    } while (/* CONSTCOND */ 0)
#define Xcheck(xs, xp)          XcheckN((xs), (xp), 1)
#define Xfree(xs, xp)           afree((xs).beg, (xs).areap)
#define Xclose(xs, xp)          aresize((xs).beg, (xp) - (xs).beg, (xs).areap)
#define Xstring(xs, xp)         ((xs).beg)
#define Xnleft(xs, xp)          ((xs).end - (xp))
#define Xlength(xs, xp)         ((xp) - (xs).beg)
#define Xsize(xs, xp)           ((xs).end - (xs).beg)
#define Xsavepos(xs, xp)        ((xp) - (xs).beg)
#define Xrestpos(xs, xp, n)     ((xs).beg + (n))

char *Xcheck_grow(XString *, const char *, size_t);

/* expandable pointer vector — an alloc-backed dynamic array of void* */
typedef struct {
    void **beg;
    size_t len;                 /* in use */
    size_t siz;                 /* allocated capacity */
} XPtrV;

#define XPinit(x, n)                                                                       \
    do {                                                                                   \
        (x).siz = (n);                                                                     \
        (x).len = 0;                                                                       \
        (x).beg = alloc2((x).siz, sizeof(void *), ATEMP);                                  \
    } while (/* CONSTCOND */ 0)

#define XPput(x, p)                                                                        \
    do {                                                                                   \
        if ((x).len == (x).siz) {                                                          \
            (x).beg = aresize2((x).beg, (x).siz, 2 * sizeof(void *), ATEMP);               \
            (x).siz <<= 1;                                                                 \
        }                                                                                  \
        (x).beg[(x).len++] = (p);                                                          \
    } while (/* CONSTCOND */ 0)

#define XPptrv(x)               ((x).beg)
#define XPsize(x)               ((x).len)
#define XPclose(x)              aresize2((x).beg, XPsize(x), sizeof(void *), ATEMP)
#define XPfree(x)               afree((x).beg, ATEMP)

/* opts for print_columns (used by select / read / etc.) */
struct columnise_opts {
    struct shf *shf;
    char        linesep;
    bool        do_last;
    bool        prefcol;
};

/* -----------------------------------------------------------------
 * 10. Pool of message strings (Txxx names).
 *     Lives in include/msgs.h — included last so the EXTERN/E_INIT
 *     machinery above is in scope.
 * ----------------------------------------------------------------- */

#include "msgs.h"

/* -----------------------------------------------------------------
 * 11. Domain headers carved out of the original sh.h.
 *     Pulled in here so existing .c files (which only ever
 *     #include "sh.h") still see the full surface.  Order matters:
 *     env.h needs Area (above), shf.h needs QSH_A_FORMAT (above)
 *     and Area, tree.h is pure data.
 * ----------------------------------------------------------------- */

#include "env.h"
#include "cclass.h"
#include "shf.h"
#include "tree.h"
#include "var.h"
#include "lex.h"
#include "proto.h"

/* ATEMP — temporary allocation area of the current env.  Defined
   here, not in env.h, so it ends up next to APERM for grep-ability
   and so env.h doesn't need to refer to the implicit `e` global. */
#define ATEMP                   (&e->area)

#endif /* _QRV_SH_H */
