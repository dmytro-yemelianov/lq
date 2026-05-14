/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Private header for the variable-handling pieces split across:
 *   var.c          — variable lookup, set/get, typeset, blocks, makenv
 *   var_special.c  — special variables (PWD, RANDOM, etc.), arrays,
 *                    window-size tracking, hash, RNG
 */
#ifndef QSH_VAR_PRIV_H
#define QSH_VAR_PRIV_H

#include "sh.h"

/* Two views of qsh_ari_t to dodge sint→uint implementation defined cast. */
typedef union {
    qsh_ari_t i;
    qsh_uari_t u;
} qsh_ari_u;

/* Special-variable IDs, derived from var_spec.h via X-macro. */
#define VARSPEC_DEFNS
#include "var_spec.h"

enum var_specs {
#define VARSPEC_ENUMS
#include "var_spec.h"
    V_MAX
};

/* Shared state */
extern struct table specials;
extern k32 lcg_state;
extern k32 qh_state;

/* var.c entry points called from var_special.c */
int getint(struct tbl *vp, qsh_ari_u *nump, bool arith);
int getnum(const char *s, qsh_ari_u *nump, bool arith, bool psxoctal);
void setint(struct tbl *vq, qsh_ari_t n);
void setint_n(struct tbl *vq, qsh_ari_t num, int newbase);
struct tbl *vtypeset(int *ep, const char *var, kui set, kui clr, int field, int base);
void c_typeset_vardump(struct tbl *vp, kui flag, int thing, int pad, bool pflag, bool istset);
void c_typeset_vardump_recursive(struct block *l, kui flag, int thing, bool pflag, bool istset);

#endif /* QSH_VAR_PRIV_H */
