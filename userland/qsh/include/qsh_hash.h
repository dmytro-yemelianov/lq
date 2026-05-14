/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSH_MIRHASH_H
#define QSH_MIRHASH_H

/*-
 * BAFH1-0 is defined by the following primitives:
 *
 * • BAFHInit(ctx) initialises the hash context, which consists of a
 *   sole 32-bit unsigned integer (ideally in a register), to 0^H1.
 *   It is possible to use any initial value out of [0; 2³²[ — which
 *   is, in fact, recommended if using BAFH for entropy distribution
 *   — but for a regular stable hash, the IV 0^H1 is needed: iff the
 *   sum of ctx and val in BAFHUpdateOctet is 0, leading NULs aren’t
 *   counted, so apply suitable bias to ensure that state is avoided.
 *
 * • BAFHUpdateOctet(ctx,val) compresses the unsigned 8-bit quantity
 *   into the hash context using Jenkins’ one-at-a-time algorithm.
 *
 * • BAFHFinish(ctx) avalanches the context around so every sub-byte
 *   depends on all input octets; afterwards, the context variable’s
 *   value is the hash output. BAFH does not use any padding, nor is
 *   the input length added; this is due to the common use case (for
 *   quick entropy distribution and use with a hashtable).
 *   Warning: BAFHFinish uses the MixColumn algorithm of AES — which
 *   is reversible (to avoid introducing funnels and reducing entro‐
 *   py), so blinding may need to be employed for some uses, e.g. in
 *   mksh, after a fork. For ctx == 0 only, ctx will be 0 afterwards.
 *
 * The following high-level macros are available:
 *
 * • BAFHUpdateMem(ctx,buf,len) adds a memory block to a context.
 * • BAFHUpdateStr(ctx,buf) is equivalent to using len=strlen(buf).
 * • BAFHUpdateVLQ(ctx,ut,ival) encodes as VLQ with negated A bit.
 *
 * All macros may use ctx multiple times in their expansion, but all
 * other arguments are always evaluated at most once.
 */

#define BAFHInit(h)                                                                                \
    do {                                                                                           \
        (h) = qiMM(k32, K32_FM, 1U);                                                              \
    } while (/* CONSTCOND */ 0)

#define BAFHUpdateOctet(h, b)                                                                      \
    do {                                                                                           \
        (h) = qiMO(k32, K32_FM, (h), +, KBI(b));                                                  \
        (h) = qiMO(k32, K32_FM, (h), +, qiMKshl(k32, K32_FM, (h), 10));                          \
        (h) = qiMO(k32, K32_FM, (h), ^, qiMKshr(k32, K32_FM, (h), 6));                           \
    } while (/* CONSTCOND */ 0)

#define BAFHFinish__impl(h, v, d)                                                                  \
    v = qiMO(k32, K32_FM, qiMKshr(k32, K32_FM, h, 7), &, 0x01010101U);                           \
    v = qiMO(k32, K32_FM, v, +, qiMKshl(k32, K32_FM, v, 1));                                     \
    v = qiMO(k32, K32_FM, v, +, qiMKshl(k32, K32_FM, v, 3));                                     \
    v = qiMO(k32, K32_FM, v, ^, qiMO(k32, K32_FM, qiMKshl(k32, K32_FM, h, 1), &, 0xFEFEFEFEU)); \
                                                                                                   \
    v = qiMO(k32, K32_FM, v, ^, qiMKror(k32, K32_FM, v, 8));                                     \
    v = qiMO(k32, K32_FM, v, ^, (h = qiMKror(k32, K32_FM, h, 8)));                               \
    v = qiMO(k32, K32_FM, v, ^, (h = qiMKror(k32, K32_FM, h, 8)));                               \
    d = qiMO(k32, K32_FM, v, ^, qiMKror(k32, K32_FM, h, 8));

#define BAFHFinish(h)                                                                              \
    do {                                                                                           \
        register k32 BAFHFinish_v;                                                                 \
                                                                                                   \
        BAFHFinish__impl((h), BAFHFinish_v, (h))                                                   \
    } while (/* CONSTCOND */ 0)

#define BAFHUpdateMem(h, p, z)                                                                     \
    do {                                                                                           \
        register const unsigned char *BAFHUpdate_p;                                                \
        register const unsigned char *BAFHUpdate_d;                                                \
                                                                                                   \
        BAFHUpdate_p = (const void *)(p);                                                          \
        BAFHUpdate_d = BAFHUpdate_p + (z);                                                         \
        while (BAFHUpdate_p < BAFHUpdate_d)                                                        \
            BAFHUpdateOctet((h), *BAFHUpdate_p++);                                                 \
    } while (/* CONSTCOND */ 0)

#define BAFHUpdateStr(h, s)                                                                        \
    do {                                                                                           \
        register const unsigned char *BAFHUpdate_s;                                                \
        register unsigned char BAFHUpdate_c;                                                       \
                                                                                                   \
        BAFHUpdate_s = (const void *)(s);                                                          \
        while ((BAFHUpdate_c = *BAFHUpdate_s++))                                                   \
            BAFHUpdateOctet((h), BAFHUpdate_c);                                                    \
    } while (/* CONSTCOND */ 0)

#define BAFHUpdateVLQ(h, t, n)                                                                     \
    do {                                                                                           \
        unsigned char BAFHUpdate_s[(sizeof(t) * (CHAR_BIT) + 6) / 7];                              \
        register size_t BAFHUpdate_n = sizeof(BAFHUpdate_s);                                       \
        t BAFHUpdate_v = (n);                                                                      \
                                                                                                   \
        do {                                                                                       \
            BAFHUpdate_s[--BAFHUpdate_n] = BAFHUpdate_v & 0x7FU;                                   \
            BAFHUpdate_v >>= 7;                                                                    \
        } while (BAFHUpdate_v);                                                                    \
        BAFHUpdate_s[sizeof(BAFHUpdate_s) - 1] |= 0x80U;                                           \
        while (BAFHUpdate_n < sizeof(BAFHUpdate_s))                                                \
            BAFHUpdateOctet((h), BAFHUpdate_s[BAFHUpdate_n++]);                                    \
    } while (/* CONSTCOND */ 0)

#endif
