/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Private header for the word-expansion pieces split across:
 *   eval.c        — public entry points (substitute, eval, evalstr,
 *                   evalonestr) and the big expand() driver
 *   eval_expand.c — varsub, comsub, trimsub, glob, debunk, tilde
 *                   handling, alt_expand, funsub, valsub
 *
 * The expand() driver and its sub-pass helpers share the Expand
 * state object and a handful of state-machine constants.
 */
#ifndef QSH_EVAL_PRIV_H
#define QSH_EVAL_PRIV_H

#include "sh.h"

/* expansion generator state — shared between expand() and its sub-passes */
typedef struct {
    /* not including an "int type;" member, see expand() */
    /* string */
    const char *str;
    /* source */
    union {
        /* string[] */
        const char **strv;
        /* file */
        struct shf *shf;
    } u;
    /* variable in ${var...} */
    struct tbl *var;
    /* split "$@" / call waitlast in $() */
    bool split;
} Expand;

/* expand() state-machine tags */
#define XBASE 0      /* scanning original string */
#define XARGSEP 1    /* ifs0 between "$*" */
#define XARG 2       /* expanding $*, $@ */
#define XCOM 3       /* expanding $() */
#define XNULLSUB 4   /* "$@" when $# is 0, so don't generate word */
#define XSUB 5       /* expanding ${} string */
#define XSUBMID 6    /* middle of expanding ${}; must be XSUB+1 */
#define XSUBPAT 7    /* expanding [[ x = ${} ]] string */
#define XSUBPATMID 8 /* middle, must be XSUBPAT+1 */
#define isXSUB(t) ((t) == XSUB || (t) == XSUBPAT)

/* substitution-type bits (used by trimsub and varsub) */
#define STYPE_CHAR 0xFF
#define STYPE_DBL 0x100
#define STYPE_AT 0x200
#define STYPE_SINGLE 0x2FF
#define STYPE_MASK 0x300

/* eval_expand.c entry points called from eval.c's expand() */
int varsub(Expand *xp, const char *sp, const char *word, unsigned int *stypep, int *slenp);
int comsub(Expand *xp, const char *cp, int fn);
char *trimsub(char *str, char *pat, int how);
void glob(char *cp, XPtrV *wp, bool markdirs);
const char *maybe_expand_tilde(const char *p, XString *dsp, char **dpp, bool isassign);
void alt_expand(XPtrV *wp, char *start, char *exp_start, char *end, int fdo);

/* eval.c entry points called from eval_expand.c */
int utflen(const char *s);
void utfincptr(const char *s, qsh_ari_t *lp);

#endif /* QSH_EVAL_PRIV_H */
