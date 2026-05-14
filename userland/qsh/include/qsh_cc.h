/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Compiler / language toolkit: compile-time assertions, flexible array
 * member helpers, stringification, predict-true/false, qccABEND hook.
 *
 * Floor: C11 + <stdint.h>.  No MSVC, Watcom, klibc, dietlibc, NWCC,
 * C++, EBCDIC, or pre-C99 fallbacks.  This header only depends on
 * <stddef.h> for size_t / offsetof.
 *
 * qccABEND must be defined by the user before #include here, since
 * we don't pull in <stdlib.h> for abort().
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SYSKERN_QSH_CC_H
#define SYSKERN_QSH_CC_H 1

#include <stddef.h>

#ifndef qccABEND
#define qccABEND(reasonstr) abort()
#endif

/* gcc branch-prediction hints (we always have gcc).  May already be
   provided by <sys/cdefs.h> or similar — guard accordingly. */
#ifndef __predict_true
#define __predict_true(exp)             __builtin_expect(!!(exp), 1)
#endif
#ifndef __predict_false
#define __predict_false(exp)            __builtin_expect(!!(exp), 0)
#endif

/* qsh_int.h replaces this with qiSIZE_MAX once SIZE_MAX is in scope */
#undef qccSIZE_MAX
#define qccSIZE_MAX                    ((size_t)~(size_t)0)

#define qcextern                       extern
#define qcextern_beg                   /* nothing */
#define qcextern_end                   /* nothing */

/* compiler-warning pragma stubs (kept as no-ops for callers) */
#define qmscWd(d)                      /* nothing */
#define qmscWs(d)                      /* nothing */
#define qmscWpop                       /* nothing */

/* in-expression compile-time check evaluating to 0 */
#define qccChkExpr(test)                                                                  \
    (sizeof(struct { unsigned int(qccChkExpr) : ((0 + (test)) ? 1 : -1); }) * 0)

/* ensure value x is a constant expression (and pass it through) */
#define qccCEX(x)                                                                         \
    (sizeof(struct { unsigned int(qccCEX) : (((0 + (x)) && 1) + 1); }) * 0 + (x))

/* flexible array member — C99/C11 mandates the empty-bracket form */
#define qccFAMslot(type, memb)         type memb[]
#define qccFAMsz_i(t, memb, sz)        ((size_t)(offsetof(t, memb) + (size_t)sz))

/* compile-time-constant FAM size */
#define qccFAMSZ(struc, memb, sz)                                                         \
    ((size_t)(qccChkExpr(qccSIZE_MAX - sizeof(struc) > qccCEX((size_t)(sz))) +          \
              (qccFAMsz_i(struc, memb, (sz)) > sizeof(struc) ? qccFAMsz_i(struc, memb,   \
                                                                           (sz))           \
                                                              : sizeof(struc))))
/* run-time FAM size */
#define qccFAMsz(struc, memb, sz)                                                         \
    ((size_t)(!(qccSIZE_MAX - sizeof(struc) > (size_t)(sz))                               \
                  ? (qccABEND("qccFAMsz: " qccS(sz) " too large for " qccS(struc) "."  \
                               qccS(memb)),                                               \
                     0)                                                                    \
                  : (qccFAMsz_i(struc, memb, (sz)) > sizeof(struc)                        \
                         ? qccFAMsz_i(struc, memb, (sz))                                  \
                         : sizeof(struc))))

/* example:
 *
 * struct s {
 *  int type;
 *  qccFAMslot(char, label);   // like char label[…];
 * };
 * struct s *sp = malloc(qccFAMsz(struct s, label, strlen(labelvar) + 1U));
 * struct s *np = malloc(qccFAMSZ(struct s, label, sizeof("myname")));
 * memcpy(np->label, "myname", sizeof("myname"));
 */

/* sizeof a struct field */
#define qccFSZ(struc, memb)            (sizeof(((struc){0}).memb))

/* stringification with expansion */
#define qccS(x)                        #x
#define qccS2(x)                       qccS(x)

/* compile-time assertion — C11 _Static_assert is the only path */
#define qccCTA(fldn, cond)             _Static_assert(cond, qccS(fldn))

/* assertion block (a CTA carrier struct, just for grouping/scoping) */
#define qCTA_BEG(name)                                                                    \
    struct ctassert_##name {                                                               \
        char t[2];                                                                         \
    } /* semicolon provided by user */
#define qCTA_END(name)                                                                    \
    struct ctassert2##name {                                                               \
        char t[sizeof(struct ctassert_##name) > 1U ? 1 : -1];                              \
    } /* semicolon provided by user */

/* single assertion */
#define qCTA(name, cond)               qccCTA(cta_##name, (cond))

/* nil pointer constant */
#define qnil                           ((void *)0)

/* prefix to suppress -Wunused-result when truly nothing can be done */
#define SHIKATANAI                      (void)

#endif /* !SYSKERN_QSH_CC_H */
