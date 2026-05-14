/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Parsing & execution environment.  Each `( cmd )` subshell, each
 * function call, each `eval`, each script `.`-source, each parser
 * recursion, gets a fresh struct env on a stack rooted at the
 * global `e`.  unwind() walks the stack via qshlongjmp() until it
 * finds the environment whose type matches the unwind reason.
 *
 * Note: qshlongjmp MUST NOT be passed 0 as the second argument.
 * qshsetjmp() does not save the signal mask; qshlongjmp() does
 * not restore it.  See doc/md/libc_signals_two_thread.md.
 */

#ifndef _QRV_SH_ENV_H
#define _QRV_SH_ENV_H

#include "sh.h"     /* Area, ALLOC_ITEM, EXTERN, BIT */

/* setjmp wrappers.  Upstream mksh used the BSD bare `_setjmp/_longjmp`
 * pair (which skip sigprocmask save/restore).  musl does not provide
 * the underscored variants, so we use POSIX `setjmp/longjmp` instead.
 * Functionally equivalent for us: there's no signal-mask juggling in
 * QSOE (signals are pulses delivered to a dedicated thread, not
 * unix-style mask-based handlers).
 *
 * Long-term plan: drop setjmp/longjmp entirely in favour of explicit
 * qsh_error_t propagation (see include/qsh_error.h and
 * doc/plans/v0.6.4-longjmp-rip.md).  Until that lands, these macros
 * remain. */
#define qshjmp_buf      jmp_buf
#define qshsetjmp(jbuf) setjmp(jbuf)
#define qshlongjmp      longjmp

/*
 * fd-save table entry.  When the shell needs to redirect an fd
 * temporarily (e.g. exec 3<file in a pipeline) it dups the
 * original to FDBASE..FDBASE+N and remembers the dup'd fd in
 * one of these bytes — top bit FDICLMASK if "originally closed",
 * low 7 bits FDNUMMASK = (savedfd - FDBASE).
 */
typedef kby qsh_fdsave;

#define FDICLMASK               ((qsh_fdsave)0x80U)
#define FDNUMMASK               ((qsh_fdsave)0x7FU)
#define FDCLOSED                ((qsh_fdsave)0x01U)

#define FDSAVE(i, sfd)                                                                     \
    do {                                                                                   \
        int FDSAVEsavedfd = (sfd);                                                         \
        e->savedfd[i] = FDSAVEsavedfd < FDBASE                                             \
                      ? FDCLOSED                                                           \
                      : (qsh_fdsave)(FDSAVEsavedfd & FDNUMMASK);                           \
    } while (/* CONSTCOND */ 0)
#define FDSVNUM(ep, i)          ((kui)((ep)->savedfd[i] & FDNUMMASK))
#define SAVEDFD(ep, i)          (FDSVNUM(ep, i) == (kui)FDCLOSED \
                                 ? -1 : (int)FDSVNUM(ep, i))

/* forward declarations — full types live in their owning headers */
struct block;                   /* var.h: local variables */
struct temp;                    /* exec.h: temp files */
struct yyrecursive_state;       /* lex.h: parser recursion */
struct sretrace_info;           /* lex.h: $(...) trace */

/* the environment itself */
extern struct env {
    ALLOC_ITEM   alloc_INT;             /* internal — do not touch */
    Area         area;                  /* temporary allocation area */
    struct env  *oenv;                  /* link to previous environment */
    struct block            *loc;       /* local variables and functions */
    qsh_fdsave              *savedfd;   /* original fds for redirected fds */
    struct temp             *temps;     /* temp files */
    struct yyrecursive_state *yyrecursive_statep;
    qshjmp_buf   jbuf;                  /* long jump back to env creator */
    kby          type;                  /* see E_* below */
    kby          flags;                 /* see EF_* below */
} *e;

EXTERN struct sretrace_info *retrace_info;
EXTERN unsigned int subshell_nesting_type;

/* env.type — '#' indicates env has a valid jbuf */
#define E_NONE   0      /* dummy environment */
#define E_PARSE  1      /* parsing command # */
#define E_FUNC   2      /* executing function # */
#define E_INCL   3      /* including a file via . # */
#define E_EXEC   4      /* executing command tree */
#define E_LOOP   5      /* executing for/while # */
#define E_ERRH   6      /* general error handler # */
#define E_GONE   7      /* hidden in child */
#define E_EVAL   8      /* running eval # */

/* env.flags */
#define EF_BRKCONT_PASS  BIT(1)         /* E_LOOP must pass break/continue on */
#define EF_FAKE_SIGDIE   BIT(2)         /* hack: pass info from unwind to quitenv */
#define EF_IN_EVAL       BIT(3)         /* inside an eval */

/* break/continue stop at env type t? */
#define STOP_BRKCONT(t)  ((t) == E_NONE || (t) == E_PARSE \
                          || (t) == E_FUNC || (t) == E_INCL)
/* return stops at env type t? */
#define STOP_RETURN(t)   ((t) == E_FUNC  || (t) == E_INCL)

/* qshlongjmp(e->jbuf, i) reason codes — MUST NOT be 0 */
#define LRETURN  1      /* return statement */
#define LEXIT    2      /* exit statement */
#define LERROR   3      /* kerrf() called */
#define LERREXT  4      /* set -e caused */
#define LINTR    5      /* ^C noticed */
#define LBREAK   6      /* break statement */
#define LCONTIN  7      /* continue statement */
#define LSHELL   8      /* return to interactive shell() */
#define LAEXPR   9      /* error in arithmetic expression */
#define LLEAVE   10     /* untrappable exit/error */
#define LRDERR   11     /* read error of script */

#endif /* _QRV_SH_ENV_H */
