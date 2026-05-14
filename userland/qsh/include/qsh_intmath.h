/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Typed/checked integer arithmetic toolkit.
 *
 * Floor: C11 + <stdint.h> + LP64.  No MSVC __int64, no 4.4BSD quad_t,
 * no CHERI, no 16-bit small-system mode, no Win32 ssize_t shim, no
 * RSIZE_MAX.  Pulls only <limits.h> + <sys/types.h>; the caller has
 * already included "qsh_cc.h" and <stdint.h> via sh.h.
 *
 * Includes (must already be in scope):
 *  <sys/types.h>       POSIX off_t / ssize_t
 *  <stdint.h>          intmax_t / uintmax_t / SIZE_MAX / PTRDIFF_MAX
 *                      / UINTPTR_MAX / INTMAX_MAX / etc.
 *  "qsh_cc.h"          qCTA, qccCTA
 *
 * cpp knobs (set in sh.h before include):
 *  QSH_INT_H_WANT_PTR_IN_SIZET=1   ensure pointers fit size_t
 *  QSH_INT_H_WANT_SIZET_IN_LONG=1  ensure size_t fits long
 *  QSH_INT_H_WANT_INT32=1          ensure unsigned int >= 32 bit
 *  QSH_INT_H_WANT_SAFEC=1          ensure two's complement
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef SYSKERN_QSH_INTMATH_H
#define SYSKERN_QSH_INTMATH_H 1

#include <limits.h>

/* ============================================================
 * Section 1.  Type-introspection / mask primitives.
 * ============================================================ */

#define qiMASK_bitmax 279

#define qiCTA_TYPE_MBIT(nm, ty)                                                           \
    qccCTA(ctatm_##nm, (sizeof(ty) <= (qiMASK_bitmax / (CHAR_BIT))))

/* type kind (compile-time and runtime) */
#define qiTYPE_ISF(type)               (!!(0 + (int)(2 * (type)0.5)))
#define qiTYPE_ISU(type)               ((type) - 1 > (type)0)

/* type limits */
#define qiTYPE_UMAX(type)              ((type) ~(type)0U)
#define qiTYPE_UBITS(type)             qiMASK_BITS(qiTYPE_UMAX(type))

/* bit-count of a (2^n - 1) value, by Hallvard B Furuseth (≤2039 bit) */
#define qiMASK__lh(maxv)               ((maxv) / ((maxv) % 255 + 1) / 255 % 255 * 8)
#define qiMASK__rh(maxv)               (7 - 86 / ((maxv) % 255 + 12))
#define qiMASK__BITS(maxv)             (qiMASK__lh(maxv) + qiMASK__rh(maxv))
#define qiMASK__type(maxv)             (qiMASK__lh(maxv) + (int)qiMASK__rh(maxv))
#define qiMASK_BITS(maxv)              (0U + (unsigned int)qiMASK__type(maxv))

/* ensure v is a positive (2^n - 1) value, up to 279 bits */
#define qiMASK_CHK(v)                  ((v) > 0 ? qi_maskchk31_1((v)) : 0)
#define qi_maskchks(v, m, o, n)        (v <= m ? o : ((v & m) == m) && n)
#define qi_maskchk31s(v, n)            qi_maskchks(v, 0x7FFFFFFFUL, qi_maskchk16(v), n)
#define qi_maskchk31_1(v)              qi_maskchk31s(v, qi_maskchk31_2(v >> 31))
#define qi_maskchk31_2(v)              qi_maskchk31s(v, qi_maskchk31_3(v >> 31))
#define qi_maskchk31_3(v)              qi_maskchk31s(v, qi_maskchk31_4(v >> 31))
#define qi_maskchk31_4(v)              qi_maskchk31s(v, qi_maskchk31_5(v >> 31))
#define qi_maskchk31_5(v)              qi_maskchk31s(v, qi_maskchk31_6(v >> 31))
#define qi_maskchk31_6(v)              qi_maskchk31s(v, qi_maskchk31_7(v >> 31))
#define qi_maskchk31_7(v)              qi_maskchk31s(v, qi_maskchk31_8(v >> 31))
#define qi_maskchk31_8(v)              qi_maskchk31s(v, qi_maskchk31_9(v >> 31))
#define qi_maskchk31_9(v)              (v <= 0x7FFFFFFFUL && qi_maskchk16(v))
#define qi_maskchk16(v)                qi_maskchks(v, 0xFFFFU, qi_maskchk8(v),          \
                                                     qi_maskchk8(v >> 16))
#define qi_maskchk8(v)                 qi_maskchks(v, 0xFFU, qi_maskchk4(v),            \
                                                     qi_maskchk4(v >> 8))
#define qi_maskchk4(v)                 qi_maskchks(v, 0xFU, qi_maskchkF(v),             \
                                                     qi_maskchkF(v >> 4))
#define qi_maskchkF(v)                 (v == 0xF || v == 7 || v == 3 || v == 1 || !v)

/* ============================================================
 * Section 2.  size_t / ssize_t / huge / pointer-uint typedefs.
 * ============================================================ */

/* keep object sizes within ptrdiff_t-representable range */
#if (SIZE_MAX) < (PTRDIFF_MAX)
#define qiSIZE_MAX                     ((size_t)SIZE_MAX)
#else
#define qiSIZE_MAX                     ((size_t)PTRDIFF_MAX)
#define qiRSZCHK                       PTRDIFF_MAX
#endif

#define qiSIZE_S                       ssize_t
#define qiSIZE_U                       size_t
#define qiSIZE_P(c)                    "z" #c
#define qiSIZE_PV(v)                   ((qiSIZE_U)(v))

/* update qsh_cc.h forward */
#undef qccSIZE_MAX
#define qccSIZE_MAX                    qiSIZE_MAX

/* widest integer (C11 mandates intmax_t in <stdint.h>) */
#define qiHUGE_S                       intmax_t
#define qiHUGE_S_MIN                   INTMAX_MIN
#define qiHUGE_S_MAX                   INTMAX_MAX
#define qiHUGE_U                       uintmax_t
#define qiHUGE_U_MAX                   UINTMAX_MAX
#define qiHUGE_P(c)                    PRI##c##MAX

/* integer that holds a pointer (C11 + LP64 → uintptr_t always present) */
#define qiPTR_U                        uintptr_t
#define qiPTR_U_MAX                    UINTPTR_MAX

/* two's-complement detection */
#undef qiSAFECOMPLEMENT
#if ((SCHAR_MIN) == -(SCHAR_MAX))
#define qiSAFECOMPLEMENT 0
#elif ((SCHAR_MIN) + 1 == -(SCHAR_MAX))
#define qiSAFECOMPLEMENT 1
#endif

/* ============================================================
 * Section 3.  Compile-time sanity checks.
 * ============================================================ */

qCTA_BEG(qsh_int_h);
/* compiler (in)sanity, from autoconf */
#define qiCTf(x) 'x'
qCTA(xlc6, qiCTf(a) == 'x');
#undef qiCTf
qCTA(osf4, '\x00' == 0);

/* C base types */
qCTA(basic_char_smask, qiMASK_CHK(SCHAR_MAX));
qCTA(basic_char_umask, qiMASK_CHK(UCHAR_MAX));
qCTA(basic_char,
      sizeof(char) == 1 && (CHAR_BIT) >= 7 && (CHAR_BIT) < 2040 &&
          (((CHAR_MAX) == (SCHAR_MAX) && (CHAR_MIN) == (SCHAR_MIN)) ||
           ((CHAR_MAX) == (UCHAR_MAX) && (CHAR_MIN) == 0)) &&
          qiTYPE_UMAX(unsigned char) == (UCHAR_MAX) && sizeof(signed char) == 1 &&
          sizeof(unsigned char) == 1 && qiTYPE_UBITS(unsigned char) >= 8 &&
          (SCHAR_MIN) < 0 && ((SCHAR_MIN) == -(SCHAR_MAX) || (SCHAR_MIN) + 1 == -(SCHAR_MAX)) &&
          qiTYPE_UBITS(unsigned char) == (unsigned int)(CHAR_BIT));
#ifndef qiSAFECOMPLEMENT
qCTA(basic_char_complement, /* fail */ 0);
#endif

qiCTA_TYPE_MBIT(short, short);
qiCTA_TYPE_MBIT(ushort, unsigned short);
qiCTA_TYPE_MBIT(int, int);
qiCTA_TYPE_MBIT(uint, unsigned int);
qiCTA_TYPE_MBIT(long, long);
qiCTA_TYPE_MBIT(ulong, unsigned long);
qiCTA_TYPE_MBIT(llong, long long);
qiCTA_TYPE_MBIT(ullong, unsigned long long);
qiCTA_TYPE_MBIT(imax, intmax_t);
qiCTA_TYPE_MBIT(uimax, uintmax_t);
qiCTA_TYPE_MBIT(uhuge, qiHUGE_U);
qiCTA_TYPE_MBIT(uptr, qiPTR_U);

qCTA(basic_short_smask, qiMASK_CHK(SHRT_MAX));
qCTA(basic_short_umask, qiMASK_CHK(USHRT_MAX));
qCTA(basic_short,
      qiTYPE_UMAX(unsigned short) == (USHRT_MAX) && qiTYPE_UBITS(unsigned short) >= 16 &&
          (SHRT_MIN) < 0 && ((SHRT_MIN) == -(SHRT_MAX) || (SHRT_MIN) + 1 == -(SHRT_MAX)) &&
          ((SHRT_MIN) == -(SHRT_MAX)) == ((SCHAR_MIN) == -(SCHAR_MAX)) &&
          sizeof(short) >= sizeof(signed char) &&
          sizeof(unsigned short) >= sizeof(unsigned char) &&
          qiMASK_BITS(SHRT_MAX) >= qiMASK_BITS(SCHAR_MAX) &&
          qiMASK_BITS(SHRT_MAX) < qiMASK_BITS(qiHUGE_S_MAX) &&
          qiMASK_BITS(USHRT_MAX) >= qiMASK_BITS(UCHAR_MAX) &&
          qiMASK_BITS(USHRT_MAX) < qiMASK_BITS(qiHUGE_U_MAX) &&
          sizeof(short) == sizeof(unsigned short));

qCTA(basic_int_smask, qiMASK_CHK(INT_MAX));
qCTA(basic_int_umask, qiMASK_CHK(UINT_MAX));
qCTA(basic_int,
      qiTYPE_UMAX(unsigned int) == (UINT_MAX) && qiTYPE_UBITS(unsigned int) >= 16 &&
          (INT_MIN) < 0 && ((INT_MIN) == -(INT_MAX) || (INT_MIN) + 1 == -(INT_MAX)) &&
          ((INT_MIN) == -(INT_MAX)) == ((SCHAR_MIN) == -(SCHAR_MAX)) &&
          sizeof(int) >= sizeof(short) &&
          sizeof(unsigned int) >= sizeof(unsigned short) &&
          qiMASK_BITS(INT_MAX) >= qiMASK_BITS(SHRT_MAX) &&
          qiMASK_BITS(INT_MAX) <= qiMASK_BITS(qiHUGE_S_MAX) &&
          qiMASK_BITS(UINT_MAX) >= qiMASK_BITS(USHRT_MAX) &&
          qiMASK_BITS(UINT_MAX) <= qiMASK_BITS(qiHUGE_U_MAX) &&
          sizeof(int) == sizeof(unsigned int));

qCTA(basic_long_smask, qiMASK_CHK(LONG_MAX));
qCTA(basic_long_umask, qiMASK_CHK(ULONG_MAX));
qCTA(basic_long,
      qiTYPE_UMAX(unsigned long) == (ULONG_MAX) && qiTYPE_UBITS(unsigned long) >= 32 &&
          (LONG_MIN) < 0 && ((LONG_MIN) == -(LONG_MAX) || (LONG_MIN) + 1 == -(LONG_MAX)) &&
          ((LONG_MIN) == -(LONG_MAX)) == ((SCHAR_MIN) == -(SCHAR_MAX)) &&
          sizeof(long) >= sizeof(int) && sizeof(unsigned long) >= sizeof(unsigned int) &&
          qiMASK_BITS(LONG_MAX) >= qiMASK_BITS(INT_MAX) &&
          qiMASK_BITS(LONG_MAX) <= qiMASK_BITS(qiHUGE_S_MAX) &&
          qiMASK_BITS(ULONG_MAX) >= qiMASK_BITS(UINT_MAX) &&
          qiMASK_BITS(ULONG_MAX) <= qiMASK_BITS(qiHUGE_U_MAX) &&
          sizeof(long) == sizeof(unsigned long));

qCTA(basic_llong_smask, qiMASK_CHK(LLONG_MAX));
qCTA(basic_llong_umask, qiMASK_CHK(ULLONG_MAX));
qCTA(basic_llong,
      qiTYPE_UMAX(unsigned long long) == (ULLONG_MAX) &&
          qiTYPE_UBITS(unsigned long long) >= 64 && (LLONG_MIN) < 0 &&
          ((LLONG_MIN) == -(LLONG_MAX) || (LLONG_MIN) + 1 == -(LLONG_MAX)) &&
          ((LLONG_MIN) == -(LLONG_MAX)) == ((SCHAR_MIN) == -(SCHAR_MAX)) &&
          sizeof(long long) >= sizeof(long) &&
          sizeof(unsigned long long) >= sizeof(unsigned long) &&
          sizeof(long long) <= sizeof(qiHUGE_S) &&
          sizeof(unsigned long long) <= sizeof(qiHUGE_U) &&
          qiMASK_BITS(LLONG_MAX) >= qiMASK_BITS(LONG_MAX) &&
          qiMASK_BITS(LLONG_MAX) <= qiMASK_BITS(qiHUGE_S_MAX) &&
          qiMASK_BITS(ULLONG_MAX) >= qiMASK_BITS(ULONG_MAX) &&
          qiMASK_BITS(ULLONG_MAX) <= qiMASK_BITS(qiHUGE_U_MAX) &&
          sizeof(long long) == sizeof(unsigned long long));

qCTA(basic_imax_smask, qiMASK_CHK(INTMAX_MAX));
qCTA(basic_imax_umask, qiMASK_CHK(UINTMAX_MAX));
qCTA(basic_imax,
      !qiTYPE_ISF(uintmax_t) && !qiTYPE_ISF(intmax_t) && qiTYPE_ISU(uintmax_t) &&
          !qiTYPE_ISU(intmax_t) && qiTYPE_UMAX(uintmax_t) == (UINTMAX_MAX) &&
          qiTYPE_UBITS(uintmax_t) >= 32 && (INTMAX_MIN) < 0 &&
          ((INTMAX_MIN) == -(INTMAX_MAX) || (INTMAX_MIN) + 1 == -(INTMAX_MAX)) &&
          ((INTMAX_MIN) == -(INTMAX_MAX)) == ((SCHAR_MIN) == -(SCHAR_MAX)) &&
          sizeof(intmax_t) >= sizeof(long) && sizeof(uintmax_t) >= sizeof(unsigned long) &&
          qiMASK_BITS(INTMAX_MAX) >= qiMASK_BITS(LLONG_MAX) &&
          qiMASK_BITS(UINTMAX_MAX) >= qiMASK_BITS(ULLONG_MAX) &&
          sizeof(intmax_t) >= sizeof(long long) &&
          sizeof(uintmax_t) >= sizeof(unsigned long long) &&
          sizeof(intmax_t) == sizeof(uintmax_t));

qCTA(basic_huge_smask, qiMASK_CHK(qiHUGE_S_MAX));
qCTA(basic_huge_umask, qiMASK_CHK(qiHUGE_U_MAX));
qCTA(basic_huge,
      !qiTYPE_ISF(qiHUGE_U) && !qiTYPE_ISF(qiHUGE_S) && qiTYPE_ISU(qiHUGE_U) &&
          !qiTYPE_ISU(qiHUGE_S) && qiTYPE_UMAX(qiHUGE_U) == (qiHUGE_U_MAX) &&
          (qiHUGE_S_MIN) < 0 &&
          ((qiHUGE_S_MIN) == -(qiHUGE_S_MAX) || (qiHUGE_S_MIN) + 1 == -(qiHUGE_S_MAX)) &&
          ((qiHUGE_S_MIN) == -(qiHUGE_S_MAX)) == ((SCHAR_MIN) == -(SCHAR_MAX)) &&
          qiMASK_BITS(qiHUGE_S_MAX) >= qiMASK_BITS(LONG_MAX) &&
          qiMASK_BITS(qiHUGE_U_MAX) >= qiMASK_BITS(ULONG_MAX) &&
          sizeof(qiHUGE_S) == sizeof(qiHUGE_U));

/* off_t (POSIX) */
qCTA(basic_offt,
      sizeof(off_t) >= sizeof(int) && sizeof(off_t) <= sizeof(qiHUGE_S) &&
          !qiTYPE_ISF(off_t) && !qiTYPE_ISU(off_t));

/* ptrdiff_t / size_t / ssize_t (C99 + POSIX) */
qCTA(basic_ptrdifft,
      sizeof(ptrdiff_t) >= sizeof(int) && sizeof(ptrdiff_t) <= sizeof(qiHUGE_S) &&
          !qiTYPE_ISF(ptrdiff_t) && !qiTYPE_ISU(ptrdiff_t));
qCTA(basic_ptrdifft_mask, qiMASK_CHK(PTRDIFF_MAX));
qCTA(basic_ptrdifft_max,
      qiMASK_BITS(PTRDIFF_MAX) >= qiMASK_BITS(INT_MAX) &&
          qiMASK_BITS(PTRDIFF_MAX) <= qiMASK_BITS(qiHUGE_S_MAX) &&
          ((qiHUGE_S)(PTRDIFF_MAX) == (qiHUGE_S)(ptrdiff_t)(PTRDIFF_MAX)));

qCTA(basic_sizet,
      sizeof(size_t) >= sizeof(unsigned int) && sizeof(size_t) <= sizeof(qiHUGE_U) &&
          !qiTYPE_ISF(size_t) && qiTYPE_ISU(size_t) &&
          qiTYPE_UBITS(size_t) >= qiMASK_BITS(UINT_MAX) &&
          qiTYPE_UBITS(size_t) <= qiMASK_BITS(qiHUGE_U_MAX));
qCTA(basic_sizet_mask, qiMASK_CHK(SIZE_MAX));
qCTA(basic_sizet_max,
      qiMASK_BITS(SIZE_MAX) >= qiMASK_BITS(USHRT_MAX) &&
          qiMASK_BITS(SIZE_MAX) <= qiTYPE_UBITS(size_t) &&
          ((qiHUGE_U)(SIZE_MAX) == (qiHUGE_U)(size_t)(SIZE_MAX)));
qCTA(basic_sizet_P,
      sizeof(size_t) <= sizeof(qiSIZE_U) && qiTYPE_UBITS(size_t) <= qiTYPE_UBITS(qiSIZE_U));

#ifdef qiRSZCHK
qCTA(smax_range, (0U + (qiRSZCHK)) <= (0U + (qiHUGE_U_MAX)));
qCTA(smax_check, ((qiHUGE_U)(qiRSZCHK) == (qiHUGE_U)(size_t)(qiRSZCHK)));
#undef qiRSZCHK
#endif

qCTA(basic_ssizet_mask, qiMASK_CHK(SSIZE_MAX));
qCTA(basic_ssizet,
      sizeof(ssize_t) == sizeof(size_t) && !qiTYPE_ISF(ssize_t) && !qiTYPE_ISU(ssize_t) &&
          qiMASK_BITS(SSIZE_MAX) >= qiMASK_BITS(INT_MAX) &&
          qiMASK_BITS(SSIZE_MAX) <= qiMASK_BITS(qiHUGE_S_MAX) &&
          ((qiHUGE_S)(SSIZE_MAX) == (qiHUGE_S)(ssize_t)(SSIZE_MAX)) &&
          qiMASK_BITS(SSIZE_MAX) < qiTYPE_UBITS(size_t));
qCTA(basic_ssizet_sizet, qiMASK_BITS(SSIZE_MAX) <= qiMASK_BITS(SIZE_MAX));

/* uintptr_t (C99) */
qCTA(basic_uintptr_mask, qiMASK_CHK(UINTPTR_MAX));
qCTA(basic_uintptr,
      sizeof(uintptr_t) >= sizeof(ptrdiff_t) && sizeof(uintptr_t) >= sizeof(size_t) &&
          sizeof(uintptr_t) <= sizeof(qiHUGE_U) && !qiTYPE_ISF(uintptr_t) &&
          qiTYPE_ISU(uintptr_t) && qiMASK_BITS(UINTPTR_MAX) == qiTYPE_UBITS(uintptr_t) &&
          ((qiHUGE_U)(UINTPTR_MAX) == (qiHUGE_U)(uintptr_t)(UINTPTR_MAX)) &&
          qiTYPE_UBITS(uintptr_t) >= qiMASK_BITS(UINT_MAX) &&
          ((qiHUGE_U)(UINTPTR_MAX) >= (qiHUGE_U)(qiSIZE_MAX)) &&
          qiTYPE_UBITS(uintptr_t) >= qiTYPE_UBITS(size_t) &&
          qiTYPE_UBITS(uintptr_t) <= qiMASK_BITS(qiHUGE_U_MAX));
qCTA(basic_uintptr_pdt, qiMASK_BITS(UINTPTR_MAX) >= qiMASK_BITS(PTRDIFF_MAX));
qCTA(basic_uintptr_sizet, qiMASK_BITS(UINTPTR_MAX) >= qiMASK_BITS(SIZE_MAX));

/* signed and unsigned of equal width */
qCTA(vbits_char, qiMASK_BITS(UCHAR_MAX) == qiMASK_BITS(SCHAR_MAX) + 1U);
qCTA(vbits_short, qiMASK_BITS(USHRT_MAX) == qiMASK_BITS(SHRT_MAX) + 1U);
qCTA(vbits_int, qiMASK_BITS(UINT_MAX) == qiMASK_BITS(INT_MAX) + 1U);
qCTA(vbits_long, qiMASK_BITS(ULONG_MAX) == qiMASK_BITS(LONG_MAX) + 1U);
qCTA(vbits_llong, qiMASK_BITS(ULLONG_MAX) == qiMASK_BITS(LLONG_MAX) + 1U);
qCTA(vbits_imax, qiMASK_BITS(UINTMAX_MAX) == qiMASK_BITS(INTMAX_MAX) + 1U);
qCTA(vbits_huge, qiMASK_BITS(qiHUGE_U_MAX) == qiMASK_BITS(qiHUGE_S_MAX) + 1U);
qCTA(vbits_size, qiTYPE_UBITS(size_t) == qiMASK_BITS(SSIZE_MAX) + 1U);
qCTA(vbits_iptr, qiMASK_BITS(UINTPTR_MAX) == qiMASK_BITS(PTRDIFF_MAX) + 1U);

/* size_t containment relationships */
qCTA(sizet_minint, sizeof(size_t) >= sizeof(int));
qCTA(sizet_minlong, sizeof(size_t) >= sizeof(long));
#ifdef QSH_INT_H_WANT_SIZET_IN_LONG
qCTA(sizet_inulong, sizeof(size_t) <= sizeof(long));
#endif
qCTA(sizet_ptrdiff, sizeof(size_t) == sizeof(ptrdiff_t));
qCTA(sizet_ssize, sizeof(size_t) == sizeof(ssize_t));
qCTA(sizet_mbiPTRU,
      sizeof(qiPTR_U) == sizeof(size_t) && qiTYPE_UBITS(qiPTR_U) == qiTYPE_UBITS(size_t));
qCTA(sizet_uintptr, sizeof(size_t) == sizeof(uintptr_t));

/* user-requested extra checks */
#ifdef QSH_INT_H_WANT_INT32
qCTA(user_int32, qiTYPE_UBITS(unsigned int) >= 32);
#endif
#ifdef QSH_INT_H_WANT_SAFEC
qCTA(user_safec, qiSAFECOMPLEMENT == 1);
#endif
#ifdef QSH_INT_H_WANT_PTR_IN_SIZET
qCTA(sizet_voidptr, sizeof(size_t) == sizeof(void *));
qCTA(sizet_sintptr, sizeof(size_t) == sizeof(int *));
qCTA(sizet_funcptr, sizeof(size_t) == sizeof(void (*)(void)));
#endif

qCTA_END(qsh_int_h);

/* ============================================================
 * Section 4.  Arithmetic toolkit (pure preprocessor, OS-agnostic).
 *
 * Key:   O = operation, A = arithmetic, M* = masking
 *        U = unsigned, S = signed, m = magnitude
 *        VZ = sign (Vorzeichen) 0=positive, 1=negative
 *        v = value, ut = unsigned type, st = signed type
 *        CA = checked arithmetic, requires #define qiCfail
 *        K = manual two's complement in unsigned vars
 *
 * Masks: HM = half mask (signed _MAX, e.g. 0x7FFFFFFFUL)
 *        FM = full mask (e.g. ULONG_MAX)
 * ============================================================ */

/* basic cast helpers */
#define qiUI(v)                        (0U + (v))
#define qiUP(ut, v)                    qiUI((ut)(v))
#define qiSP(st, v)                    (0 + ((st)(v)))

/* masking */
#define qiMM(ut, tM, v)                qiOU(ut, (v), &, (tM))

/* unary op */
#define qiOS1(st, op, v)               ((st)(op qiSP(st, (v))))
#define qiOU1(ut, op, v)               ((ut)(op qiUP(ut, (v))))
#define qiMO1(ut, tM, op, v)           qiMM(ut, (tM), qiOU1(ut, op, (v)))

/* binary op / comparison */
#define qiOS(st, l, op, r)             ((st)(qiSP(st, (l)) op qiSP(st, (r))))
#define qiOU(ut, l, op, r)             ((ut)(qiUP(ut, (l)) op qiUP(ut, (r))))
#define qiMO(ut, tM, l, op, r)         qiMM(ut, (tM), qiOU(ut, (l), op, (r)))

#define qiOshl(ut, l, r)               ((ut)(qiUP(ut, (l)) << (r)))
#define qiOshr(ut, l, r)               ((ut)(qiUP(ut, (l)) >> (r)))

#define qiCOS(st, l, op, r)            (!!(qiSP(st, (l)) op qiSP(st, (r))))
#define qiCOU(ut, l, op, r)            (!!(qiUP(ut, (l)) op qiUP(ut, (r))))

/* ternary */
#define qiOT(t, w, j, n)               ((w) ? (t)(j) : (t)(n))
#define qiMOT(ut, tM, w, j, n)         qiMM(ut, (tM), qiOT(ut, (w), (j), (n)))

/* helpers */
#define qiOUneg(ut, v)                 qiOU1(ut, -, (v))

/* manual two's complement in unsigned arithmetic */

/* 1. obtain sign of encoded value */
#define qiA_S2VZ(v)                    ((v) < 0)
#define qiA_U2VZ(ut, HM, v)            qiCOU(ut, (v), >, (HM))
#define qiMA_U2VZ(ut, FM, HM, v)       qiCOU(ut, qiMM(ut, (FM), (v)), >, (HM))

/* 2. cast between unsigned(two's complement) and signed(native) */
#define qiA_U2S(ut, st, HM, v)                                                            \
    qiOT(st, qiA_U2VZ(ut, (HM), (v)),                                                    \
          qiOS(st, qiOS1(st, -, qiOU1(ut, ~, (v))), -, 1), (v))
#define qiMA_U2S(ut, st, FM, HM, v)                                                       \
    qiOT(st, qiMA_U2VZ(ut, (FM), (HM), (v)),                                             \
          qiOS(st, qiOS1(st, -, qiMO1(ut, (HM), ~, (v))), -, 1), qiMM(ut, (HM), (v)))

#define qiA_S2U(ut, st, v)                                                                \
    qiOT(ut, qiA_S2VZ(v), qiOU1(ut, ~, qiOS1(st, -, qiOS(st, (v), +, 1))), (v))
#define qiMA_S2U(ut, st, FM, v)        qiMM(ut, (FM), qiA_S2U(ut, st, (v)))

/* 3. signed(native) or unsigned(manual) → magnitude */
#define qiA_S2M(ut, st, v)                                                                \
    qiOT(ut, qiA_S2VZ(v),                                                                \
          qiOU(ut, qiOS1(st, -, qiOS(st, (v), +, 1)), +, 1U), (v))
#define qiMA_S2M(ut, st, HM, v)                                                           \
    qiOT(ut, qiA_S2VZ(v),                                                                \
          qiOU(ut, qiMM(ut, (HM), qiOS1(st, -, qiOS(st, (v), +, 1))), +, 1U),          \
          qiMM(ut, (HM), (v)))

#define qiA_U2M(ut, HM, v)                                                                \
    qiOT(ut, qiA_U2VZ(ut, (HM), (v)), qiOUneg(ut, (v)), (v))
#define qiMA_U2M(ut, FM, HM, v)                                                           \
    qiOT(ut, qiMA_U2VZ(ut, (FM), (HM), (v)),                                             \
          qiOU(ut, qiMO1(ut, (HM), ~, (v)), +, 1U), qiMM(ut, (HM), (v)))

/* 4a. signbit + magnitude → signed(native) */
#define qiA_VZM2S(ut, st, vz, m)                                                          \
    qiOT(st, (vz) && ((m) > 0U),                                                          \
          qiOS(st, qiOS1(st, -, qiOU(ut, (m), -, 1U)), -, 1), (m))
#define qiMA_VZM2S(ut, st, FM, HM, vz, m)                                                 \
    qiOT(st, (vz) && (qiMM(ut, (FM), (m)) > 0U),                                         \
          qiOS(st, qiOS1(st, -, qiMO(ut, (HM), (m), -, 1U)), -, 1),                     \
          qiMM(ut, (HM), (m)))

/* 4b. signbit + magnitude → unsigned(two's complement) */
#define qiA_VZM2U(ut, HM, vz, m)                                                          \
    qiOT(ut, (vz) && ((m) > 0U),                                                          \
          qiOU1(ut, ~, qiMO(ut, (HM), (m), -, 1U)), qiMM(ut, (HM), (m)))
#define qiMA_VZM2U(ut, FM, HM, vz, m)                                                     \
    qiOT(ut, (vz) && (qiMM(ut, (FM), (m)) > 0U),                                         \
          qiMO1(ut, (FM), ~, qiMO(ut, (HM), (m), -, 1U)), qiMM(ut, (HM), (m)))

/* 4c. signbit + unsigned → unsigned(two's complement)  (= NEG) */
#define qiA_VZU2U(ut, vz, m)           qiOT(ut, (vz), qiOUneg(ut, (m)), (m))
#define qiMA_VZU2U(ut, FM, vz, u)      qiMM(ut, (FM), qiA_VZU2U(ut, (vz), (u)))

/* ============================================================
 * Section 5.  UB-safe overflow-checking integer arithmetic.
 *
 * Before using, #define qiCfail to the action to take on overflow
 * (e.g. "goto fail" or "return (0)").  #undef qiCfail at the end of
 * the function body.
 * ============================================================ */

/* internal helpers */
#define qi__totypeof(templval, dstval) ((1 ? 0 : (templval)) + (dstval))
#define qi__halftmax(stmax)                                                               \
    qi__totypeof(stmax, (qi__totypeof(stmax, 1) << (qiMASK_BITS(stmax) / 2U)))
/* must constant-evaluate even when ut is signed */
#define qi__halftype(ut)                                                                  \
    qiOT(ut, qiTYPE_ISU(ut), qiOshl(ut, 1U, qiTYPE_UBITS(ut) / 2U), 2)
#define qi__uabovehalftype(ut, a, b)   (((ut)a | (ut)b) >= qi__halftype(ut))
#define qi__sabovehalftype(stmax, a, b)                                                   \
    (a < 0 ? 1 : (qi__totypeof(stmax, a) | qi__totypeof(stmax, b)) >= qi__halftmax(stmax))

/* unsigned: check after the operation */
#define qiCAUinc(vl)                   qiCAUadd(vl, 1U)
#define qiCAUdec(vl)                   qiCAUsub(vl, 1U)
#define qiCAUadd(vl, vr)                                                                  \
    do {                                                                                   \
        (vl) += qiUI(vr);                                                                 \
        if (__predict_false(qiUI(vl) < qiUI(vr)))                                        \
            qiCfail;                                                                      \
    } while (/* CONSTCOND */ 0)
#define qiCAUsub(vl, vr)                                                                  \
    do {                                                                                   \
        if (__predict_false(qiUI(vl) < qiUI(vr)))                                        \
            qiCfail;                                                                      \
        (vl) -= qiUI(vr);                                                                 \
    } while (/* CONSTCOND */ 0)
#define qiCAUmul(ut, vl, vr)                                                              \
    do {                                                                                   \
        if (__predict_false((vr) != 0 && qi__uabovehalftype(ut, (vl), (vr)) &&            \
                            qiOU(ut, qiTYPE_UMAX(ut), /, (ut)(vr)) < (ut)(vl)))          \
            qiCfail;                                                                      \
        (vl) *= (vr);                                                                      \
    } while (/* CONSTCOND */ 0)

/* signed: lim is the prefix (e.g. SHRT, INT) */
#define qiCASinc(lim, vl)                                                                 \
    do {                                                                                   \
        if (__predict_false((vl) == lim##_MAX))                                            \
            qiCfail;                                                                      \
        ++(vl);                                                                            \
    } while (/* CONSTCOND */ 0)
#define qiCASdec(lim, vl)                                                                 \
    do {                                                                                   \
        if (__predict_false((vl) == lim##_MIN))                                            \
            qiCfail;                                                                      \
        --(vl);                                                                            \
    } while (/* CONSTCOND */ 0)
/* CAP* assumes vr is positive */
#define qiCAPadd(lim, vl, vr)                                                             \
    do {                                                                                   \
        if (__predict_false((vl) > (lim##_MAX - (vr))))                                    \
            qiCfail;                                                                      \
        (vl) += (vr);                                                                      \
    } while (/* CONSTCOND */ 0)
#define qiCAPsub(lim, vl, vr)                                                             \
    do {                                                                                   \
        if (__predict_false((vl) < (lim##_MIN + (vr))))                                    \
            qiCfail;                                                                      \
        (vl) -= (vr);                                                                      \
    } while (/* CONSTCOND */ 0)
#define qiCAPmul(lim, vl, vr)                                                             \
    do {                                                                                   \
        if (__predict_false((vr) != 0 && qi__sabovehalftype(lim##_MAX, (vl), (vr)) &&     \
                            ((vl) < 0 ? (vl) < (lim##_MIN / (vr)) : (vl) > (lim##_MAX /    \
                                                                            (vr)))))       \
            qiCfail;                                                                      \
        (vl) *= (vr);                                                                      \
    } while (/* CONSTCOND */ 0)
/* CAS* may have negative vr */
#define qiCASadd(lim, vl, vr)                                                             \
    do {                                                                                   \
        if (__predict_false((vr) < 0 ? (vl) < (lim##_MIN - (vr))                           \
                                     : (vl) > (lim##_MAX - (vr))))                         \
            qiCfail;                                                                      \
        (vl) += (vr);                                                                      \
    } while (/* CONSTCOND */ 0)
#define qiCASsub(lim, vl, vr)                                                             \
    do {                                                                                   \
        if (__predict_false((vr) < 0 ? (vl) > (lim##_MAX + (vr))                           \
                                     : (vl) < (lim##_MIN + (vr))))                         \
            qiCfail;                                                                      \
        (vl) -= (vr);                                                                      \
    } while (/* CONSTCOND */ 0)
#define qiCASmul(lim, vl, vr)                                                             \
    do {                                                                                   \
        if (((vr) >= 0)                                                                    \
                ? __predict_false(                                                         \
                      (vr) != 0 && qi__sabovehalftype(lim##_MAX, (vl), (vr)) &&           \
                      ((vl) < 0 ? (vl) < (lim##_MIN / (vr)) : (vl) > (lim##_MAX / (vr))))  \
                : /* vr < 0 */ __predict_false(                                            \
                      (vl) < 0 ? (vl) < (lim##_MAX / (vr))                                 \
                               : (vr) != -1 && (vl) > (lim##_MIN / (vr))))                 \
            qiCfail;                                                                      \
        (vl) *= (vr);                                                                      \
    } while (/* CONSTCOND */ 0)

/* assignment with implementation-defined narrowing check */
#define qiCAAlet(vl, srctype, vr)                                                         \
    do {                                                                                   \
        (vl) = (vr);                                                                       \
        if (__predict_false((srctype)(vl) != (srctype)(vr)))                               \
            qiCfail;                                                                      \
    } while (/* CONSTCOND */ 0)
#define qiCASlet(dsttype, vl, srctype, vr)                                                \
    do {                                                                                   \
        (vl) = (dsttype)(vr);                                                              \
        if (__predict_false((srctype)(vl) != (srctype)(vr)))                               \
            qiCfail;                                                                      \
    } while (/* CONSTCOND */ 0)

/* safe-to-use qiA_U2S / qiA_VZM2S for the given in-range value */
#if qiSAFECOMPLEMENT
#define qiCAsafeU2S(lim, ut, v)                                                           \
    do {                                                                                   \
        (void)(ut)(v);                                                                     \
        (void)(lim##_MAX);                                                                 \
    } while (/* CONSTCOND */ 0)
#define qiCAsafeVZM2S(lim, ut, vz, m)                                                     \
    do {                                                                                   \
        (void)(ut)(m);                                                                     \
        (void)(lim##_MAX);                                                                 \
        (void)(vz);                                                                        \
    } while (/* CONSTCOND */ 0)
#else /* one's complement / sign-and-magnitude / -type_MAX-1 traps */
#define qiCAsafeU2S(lim, ut, v)                                                           \
    do {                                                                                   \
        if (__predict_false((ut)(v) == qiOU(ut, lim##_MAX, +, 1)))                        \
            qiCfail;                                                                      \
    } while (/* CONSTCOND */ 0)
#define qiCAsafeVZM2S(lim, ut, vz, m)                                                     \
    do {                                                                                   \
        if ((vz) && __predict_false((ut)(m) == qiOU(ut, lim##_MAX, +, 1)))                \
            qiCfail;                                                                      \
    } while (/* CONSTCOND */ 0)
#endif

/* ============================================================
 * Section 6.  Manual two's-complement K-arithmetic in unsigned vars.
 *
 * Add / sub / mul / bitwise / boolean / equality work directly on the
 * unsigned value.  Compare / shift / rotate / div / rem need K-helpers.
 * ============================================================ */

/* k-signed compare: < <= > >= */
#define qiK_signbit(ut, HM)            qiOU(ut, (HM), +, 1)
#define qiK_signflip(ut, HM, v)        qiOU(ut, (v), ^, qiK_signbit(ut, (HM)))
#define qiKcmp(ut, HM, vl, op, vr)                                                        \
    qiCOU(ut, qiK_signflip(ut, (HM), (vl)), op, qiK_signflip(ut, (HM), (vr)))
#define qiMKcmp(ut, FM, HM, vl, op, vr)                                                   \
    qiKcmp(ut, (HM), qiMM(ut, (FM), (vl)), op, qiMM(ut, (FM), (vr)))

/* rotate / shift */
#define qiKrol(ut, vl, vr)             qiK_sr(ut, qiK_rol, (vl), (vr), void)
#define qiKror(ut, vl, vr)             qiK_sr(ut, qiK_ror, (vl), (vr), void)
#define qiKshl(ut, vl, vr)             qiK_sr(ut, qiK_shl, (vl), (vr), void)
/* vz must be sgn(vl), e.g. via qi{,M}A_U2VZ(ut,HM,vl) */
#define qiKsar(ut, vz, vl, vr)         qiK_sr(ut, qiK_sar, (vl), (vr), (vz))
#define qiKshr(ut, vl, vr)             qiK_sr(ut, qiK_shr, (vl), (vr), 0)
#define qiMKrol(ut, FM, vl, vr)        qiMK_sr(ut, (FM), qiK_rol, (vl), (vr), void)
#define qiMKror(ut, FM, vl, vr)        qiMK_sr(ut, (FM), qiK_ror, (vl), (vr), void)
#define qiMKshl(ut, FM, vl, vr)        qiMK_sr(ut, (FM), qiK_shl, (vl), (vr), void)
#define qiMKsar(ut, FM, vz, l, r)                                                         \
    qiMOT(ut, (FM), (vz), qiK_SR(ut, qiMASK_BITS(FM), qiMK_nr, (l), (r), (FM)),        \
           qiK_SR(ut, qiMASK_BITS(FM), qiK_shr, (l), (r), 0))
#define qiMKshr(ut, FM, vl, vr)        qiMK_sr(ut, (FM), qiK_shr, (vl), (vr), 0)
/* implementation */
#define qiK_sr(ut, n, vl, vr, vz)      qiK_SR(ut, qiTYPE_UBITS(ut), n, vl, vr, vz)
#define qiMK_sr(ut, FM, n, l, r, z)                                                       \
    qiMM(ut, (FM), qiK_SR(ut, qiMASK_BITS(FM), n, qiMM(ut, (FM), (l)), (r), (z)))
#define qiK_SR(ut, b, n, l, r, vz)     qiK_RS(ut, b, n, l, qiUI(r) % qiUI(b), (vz))
#define qiK_RS(ut, b, n, v, cl, zx)    qiOT(ut, cl, n(ut, v, cl, b - (cl), zx), v)
#define qiK_shl(ut, ax, cl, CL, z)     qiOshl(ut, ax, cl)
#define qiK_shr(ut, ax, cl, CL, z)     qiOshr(ut, ax, cl)
#define qiK_sar(ut, ax, cl, CL, z)                                                        \
    qiOT(ut, z, qiOU1(ut, ~, qiOshr(ut, qiOU1(ut, ~, ax), cl)), qiOshr(ut, ax, cl))
#define qiMK_nr(ut, ax, cl, CL, m)                                                        \
    qiOU1(ut, ~, qiOshr(ut, qiMO1(ut, m, ~, ax), cl))
#define qiK_rol(ut, ax, cl, CL, z)     qiOU(ut, qiOshl(ut, ax, cl), |, qiOshr(ut, ax, CL))
#define qiK_ror(ut, ax, cl, CL, z)     qiOU(ut, qiOshr(ut, ax, cl), |, qiOshl(ut, ax, CL))

/* k-signed division and remainder */
#define qiKdiv(ut, HM, vl, vr)                                                            \
    qiA_VZU2U(ut, qiA_U2VZ(ut, (HM), (vl)) ^ qiA_U2VZ(ut, (HM), (vr)),                  \
               qiUI(qiA_U2M(ut, (HM), (vl))) / qiUI(qiA_U2M(ut, (HM), (vr))))
#define qiMK_div(ut, FM, HM, vl, vr)                                                      \
    qiA_VZU2U(ut, qiMA_U2VZ(ut, (FM), (HM), (vl)) ^ qiMA_U2VZ(ut, (FM), (HM), (vr)),    \
               qiUI(qiMA_U2M(ut, (FM), (HM), (vl))) /                                    \
                   qiUI(qiMA_U2M(ut, (FM), (HM), (vr))))
#define qiMKdiv(ut, FM, HM, vl, vr)    qiMM(ut, (FM), qiMK_div(ut, (FM), (HM), (vl), (vr)))
#define qiK_rem(ut, vl, vr, vdiv)      qiOU(ut, vl, -, qiOU(ut, vdiv, *, vr))
#define qiKrem(ut, HM, vl, vr)                                                            \
    qiK_rem(ut, (vl), (vr), qiKdiv(ut, (HM), (vl), (vr)))
#define qiMKrem(ut, FM, HM, vl, vr)                                                       \
    qiMM(ut, (FM), qiK_rem(ut, (vl), (vr), qiMK_div(ut, (FM), (HM), (vl), (vr))))
/* statement form, assigning to dstdiv and dstrem */
#define qiKdivrem(dstdiv, dstrem, ut, HM, vl, vr)                                         \
    do {                                                                                   \
        ut qi__TMP = qiKdiv(ut, (HM), (vl), (vr));                                       \
        (dstdiv) = qi__TMP;                                                               \
        (dstrem) = qiK_rem(ut, (vl), (vr), qi__TMP);                                     \
    } while (/* CONSTCOND */ 0)
#define qiMKdivrem(dstdiv, dstrem, ut, FM, HM, vl, vr)                                    \
    do {                                                                                   \
        ut qi__TMP = qiMK_div(ut, (FM), (HM), (vl), (vr));                               \
        (dstdiv) = qiMM(ut, (FM), qi__TMP);                                              \
        (dstrem) = qiMM(ut, (FM), qiK_rem(ut, (vl), (vr), qi__TMP));                    \
    } while (/* CONSTCOND */ 0)

#endif /* !SYSKERN_QSH_INTMATH_H */
