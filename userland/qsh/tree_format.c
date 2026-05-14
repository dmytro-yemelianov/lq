/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

#define INDENT 8

void
uprntc(unsigned char c, struct shf *shf)
{
    unsigned char a;

    if (ctype(c, C_PRINT)) {
    doprnt:
        shf_putc(c, shf);
        return;
    }

    if (!UTFMODE) {
        if (!qsh_isctrl8(c))
            goto doprnt;
        if ((a = rtt2asc(c)) >= 0x80U) {
            shf_scheck(3, shf);
            shf_putc('^', shf);
            shf_putc('!', shf);
            a &= 0x7FU;
            goto unctrl;
        }
    } else if ((a = rtt2asc(c)) >= 0x80U) {
        shf_scheck(4, shf);
        shf_putc('\\', shf);
        shf_putc('x', shf);
        shf_putc(digits_uc[(a >> 4) & 0x0F], shf);
        shf_putc(digits_uc[a & 0x0F], shf);
        return;
    }
    shf_scheck(2, shf);
    shf_putc('^', shf);
unctrl:
    shf_putc(asc2rtt(a ^ 0x40U), shf);
}

/*
 * For now, these are only used by the edit code, which requires
 * a difference: tab is output as three spaces, not as control
 * character in caret notation. If these will ever be used else‐
 * where split them up.
 */
/*size_t*/ void
uescmbT(unsigned char *dst, const char **cpp)
{
    unsigned char c;
    unsigned int wc;
    size_t n, dstsz = 0;
    const char *cp = *cpp;

    c = *cp++;
    /* test cheap first (easiest) */
    if (ctype(c, C_PRINT)) {
    prntb:
        dst[dstsz++] = c;
        goto out;
    }
    /* differently from uprntc, note tab as three spaces */
    if (ord(c) == CTRL_I) {
        dst[dstsz++] = ' ';
        dst[dstsz++] = ' ';
        dst[dstsz++] = ' ';
        goto out;
    }

    /* more specialised tests depend on shell state */
    if (UTFMODE) {
        /* wc = rtt2asc(c) except UTF-8 is decoded */
        if ((n = utf_mbtowc(&wc, cp - 1)) == (size_t)-1) {
            /* failed: invalid UTF-8 */
            wc = rtt2asc(c);
            dst[dstsz++] = '\\';
            dst[dstsz++] = 'x';
            dst[dstsz++] = digits_uc[(wc >> 4) & 0x0F];
            dst[dstsz++] = digits_uc[wc & 0x0F];
            goto out;
        }
        /*
         * printable as-is? U+0020‥U+007E already handled
         * above as they are C_PRINT, U+00A0 or higher are
         * not escaped either, anything in between is special
         */
        if (wc >= 0xA0U) {
            dst[dstsz++] = c;
            switch (n) {
#ifdef notyet
            case 4:
                dst[dstsz++] = *cp++;
                /* FALLTHROUGH */
#endif
            case 3:
                dst[dstsz++] = *cp++;
                /* FALLTHROUGH */
            default:
                dst[dstsz++] = *cp++;
                break;
            }
            goto out;
        }
        /* and encoded with either 1 or 2 octets */

        /* C1 control character, UTF-8 encoded */
        if (wc >= 0x80U) {
            /* n == 2 so we miss one out */
            ++cp;

            c = '+';
            goto prntC1;
        }
        /* nope, must be C0 or DEL */
        /* n == 1 so cp needs no adjustment */
        goto prntC0;
    }

    /* not UTFMODE allows more but the test is more expensive */
    if (!qsh_isctrl8(c))
        goto prntb;
    /* UTF-8 is not decoded, we just transfer an octet to ASCII */
    wc = rtt2asc(c);
    /* C1 control character octet? */
    if (wc >= 0x80U) {
        c = '!';
    prntC1:
        dst[dstsz++] = '^';
        dst[dstsz++] = c;
        wc &= 0x7FU;
    } else {
        /* nope, so C0 or DEL, anything else went to prntb */
    prntC0:
        dst[dstsz++] = '^';
    }
    dst[dstsz++] = asc2rtt(wc ^ 0x40U);

out:
    *cpp = cp;
    dst[dstsz] = '\0';
#ifdef usedoutsideofedit
    return (dstsz);
#endif
}

int
uwidthmbT(char *cp, char **dcp)
{
    unsigned char c;
    unsigned int wc;
    int w;
    size_t n;

    c = *cp++;
    /* test cheap first (easiest) */
    if (ctype(c, C_PRINT)) {
    prntb:
        w = 1;
        goto out;
    }
    /* differently from uprntc, note tab as three spaces */
    if (ord(c) == CTRL_I) {
        w = 3;
        goto out;
    }

    /* more specialised tests depend on shell state */
    if (UTFMODE) {
        /* wc = rtt2asc(c) except UTF-8 is decoded */
        if ((n = utf_mbtowc(&wc, cp - 1)) == (size_t)-1) {
            /* \x## */
            w = 4;
            goto out;
        }
        cp += n - 1;
        /*
         * printable as-is? U+0020‥U+007E already handled
         * above as they are C_PRINT, U+00A0 or higher are
         * not escaped either, anything in between is special
         */
        if (wc >= 0xA0U) {
            w = utf_wcwidth(wc);
            goto out;
        }
        /* and encoded with either 1 or 2 octets */

        /* C1 control character, UTF-8 encoded */
        if (wc >= 0x80U)
            goto prntC1;
        /* nope, must be C0 or DEL */
        goto prntC0;
    }

    /* not UTFMODE allows more but the test is more expensive */
    if (!qsh_isctrl8(c))
        goto prntb;
    /* UTF-8 is not decoded, we just transfer an octet to ASCII */
    wc = rtt2asc(c);
    /* C1 control character octet? */
    if (wc >= 0x80U) {
    prntC1:
        w = 3;
    } else {
        /* nope, so C0 or DEL, anything else went to prntb */
    prntC0:
        w = 2;
    }

out:
    if (dcp)
        *dcp = cp;
    return (w);
}

const char *
uprntmbs(const char *cp, bool esc_caret, struct shf *shf)
{
    unsigned char c;
    unsigned int wc;
    size_t n;

    while ((c = *cp++) != 0) {
        /* test cheap first (easiest) */
        if (ctype(c, C_PRINT)) {
            if (esc_caret && (c == ORD('\\') || c == ORD('^'))) {
                shf_scheck(2, shf);
                shf_putc('\\', shf);
            }
        prntb:
            shf_putc(c, shf);
            continue;
        }

        /* more specialised tests depend on shell state */
        if (UTFMODE) {
            /* wc = rtt2asc(c) except UTF-8 is decoded */
            if ((n = utf_mbtowc(&wc, cp - 1)) == (size_t)-1) {
                /* failed: invalid UTF-8 */
                wc = rtt2asc(c);
                shf_scheck(4, shf);
                shf_putc('\\', shf);
                shf_putc('x', shf);
                shf_putc(digits_uc[(wc >> 4) & 0x0F], shf);
                shf_putc(digits_uc[wc & 0x0F], shf);
                continue;
            }
            /*
             * printable as-is? U+0020‥U+007E already handled
             * above as they are C_PRINT, U+00A0 or higher are
             * not escaped either, anything in between is special
             */
            if (wc >= 0xA0U) {
                --cp;
                shf_wr_sm(cp, n, shf);
                continue;
            }
            /* and encoded with either 1 or 2 octets */

            /* C1 control character, UTF-8 encoded */
            if (wc >= 0x80U) {
                /* n == 2 so we miss one out */
                ++cp;

                c = '+';
                goto prntC1;
            }
            /* nope, must be C0 or DEL */
            /* n == 1 so cp needs no adjustment */
            goto prntC0;
        }

        /* not UTFMODE allows more but the test is more expensive */
        if (!qsh_isctrl8(c))
            goto prntb;
        /* UTF-8 is not decoded, we just transfer an octet to ASCII */
        wc = rtt2asc(c);
        /* C1 control character octet? */
        if (wc >= 0x80U) {
            c = '!';
        prntC1:
            shf_scheck(3, shf);
            shf_putc('^', shf);
            shf_putc(c, shf);
            wc &= 0x7FU;
        } else {
            /* nope, so C0 or DEL, anything else went to prntb */
        prntC0:
            shf_scheck(2, shf);
            shf_putc('^', shf);
        }
        shf_putc(asc2rtt(wc ^ 0x40U), shf);
    }
    /* point to the trailing NUL for continuation */
    return ((const void *)(cp - 1));
}

