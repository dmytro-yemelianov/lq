/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Character-class machinery — mksh's own ctype-equivalent.  A
 * 256-entry table (`qsh_ctypes`) holds a 30-bit mask per byte; each
 * Ci* bit identifies a fine-grained char class (digit, upper,
 * lower, hex letter, IFS, individual punctuation char ...).
 * The C_* macros above the line are coarser unions of Ci* bits
 * (C_DIGIT, C_PRINT, C_PUNCT, etc.) — what callers normally use.
 *
 * Chosen over plain <ctype.h> because mksh distinguishes things
 * the C library doesn't: $IFS membership, individual punctuation
 * chars relevant to the lexer, etc.  Every test is one bitwise AND.
 *
 * The tables are populated by misc.c at startup; entries change
 * only when set_ifs() runs.
 */

#ifndef _QRV_SH_CCLASS_H
#define _QRV_SH_CCLASS_H

#include "sh.h"     /* BIT, ord, KBI, KBY, k32, bool, EXTERN */

/* -----------------------------------------------------------------
 * Fine-grained class bits (Ci*) — internal, normally used only via
 * the C_* unions below.  CiIFS is the dynamic one — set_ifs() flips
 * it on and off as the shell variable IFS changes.
 * ----------------------------------------------------------------- */

#define CiIFS    BIT(0)
#define CiCNTRL  BIT(1)     /* \x01..\x08 \x0E..\x1F \x7F */
#define CiUPPER  BIT(2)     /* A..Z */
#define CiLOWER  BIT(3)     /* a..z */
#define CiHEXLT  BIT(4)     /* A..F a..f */
#define CiOCTAL  BIT(5)     /* 0..7 */
#define CiQCL    BIT(6)     /* &();| */
#define CiALIAS  BIT(7)     /* !,.@ */
#define CiQCX    BIT(8)     /* *[\\~ */
#define CiVAR1   BIT(9)     /* !*@ */
#define CiQCM    BIT(10)    /* /^ */
#define CiDIGIT  BIT(11)    /* 89 */
#define CiQC     BIT(12)    /* "' */
#define CiSPX    BIT(13)    /* \x0B \x0C */
#define CiCURLY  BIT(14)    /* {} */
#define CiANGLE  BIT(15)    /* <> */
#define CiNUL    BIT(16)    /* \x00 */
#define CiTAB    BIT(17)    /* \x09 */
#define CiNL     BIT(18)    /* \x0A */
#define CiCR     BIT(19)    /* \x0D */
#define CiSP     BIT(20)    /* \x20 */
#define CiHASH   BIT(21)    /* # */
#define CiSS     BIT(22)    /* $ */
#define CiPERCT  BIT(23)    /* % */
#define CiPLUS   BIT(24)    /* + */
#define CiMINUS  BIT(25)    /* - */
#define CiCOLON  BIT(26)    /* : */
#define CiEQUAL  BIT(27)    /* = */
#define CiQUEST  BIT(28)    /* ? */
#define CiBRACK  BIT(29)    /* [] */
#define CiUNDER  BIT(30)    /* _ */
#define CiGRAVE  BIT(31)    /* ` */

/* -----------------------------------------------------------------
 * Coarse classes — C_* unions over Ci* bits.  These are what
 * almost all callers test.
 * ----------------------------------------------------------------- */

/* alphanumeric (no underscore) */
#define C_ALNUM  (CiDIGIT | CiLOWER | CiOCTAL | CiUPPER)
/* characters allowed in alias names: !%+,-.0-9:@A-Z[]_a-z */
#define C_ALIAS  (CiALIAS | CiBRACK | CiCOLON | CiDIGIT | CiLOWER | CiMINUS | \
                  CiOCTAL | CiPERCT | CiPLUS | CiUNDER | CiUPPER)
/* 7-bit ASCII except NUL */
#define C_ASCII  (C_GRAPH | CiCNTRL | CiCR | CiNL | CiSP | CiSPX | CiTAB)
/* alphanumeric + underscore */
#define C_ALNUX  (CiDIGIT | CiLOWER | CiOCTAL | CiUNDER | CiUPPER)
/* alphabetical (upper + lower) */
#define C_ALPHA  (CiLOWER | CiUPPER)
/* alphabetical + underscore (identifier lead) */
#define C_ALPHX  (CiLOWER | CiUNDER | CiUPPER)
/* tab + space */
#define C_BLANK  (CiSP | CiTAB)
/* POSIX control characters */
#define C_CNTRL  (CiCNTRL | CiCR | CiNL | CiNUL | CiSPX | CiTAB)
/* decimal digits 0..9 */
#define C_DIGIT  (CiDIGIT | CiOCTAL)
/* editor x_locate_word() command chars */
#define C_EDCMD  (CiGRAVE | CiQCL)
/* glob escape chars */
#define C_EDGLB  (CiGRAVE | CiQCX | CiQUEST | CiSS)
/* editor non-word characters */
#define C_EDNWC  (CiANGLE | CiCOLON | CiEQUAL | CiGRAVE | CiNL | CiQC | CiQCL | CiSP | CiTAB)
/* editor quotes for tab completion */
#define C_EDQ    (CiANGLE | CiCOLON | CiCURLY | CiEQUAL | CiGRAVE | CiHASH | \
                  CiQC | CiQCL | CiQCX | CiQUEST | CiSS)
/* POSIX graphical chars (alnum + punct) */
#define C_GRAPH  (C_PUNCT | CiDIGIT | CiLOWER | CiOCTAL | CiUPPER)
/* hex letter */
#define C_HEXLT  CiHEXLT
/* IFS NUL */
#define C_IFS    (CiIFS | CiNUL)
/* IFS whitespace candidates */
#define C_IFSWS  (CiNL | CiSP | CiTAB)
/* lexer separators: \t \n space & ( ) ; < > | */
#define C_LEX1   (CiANGLE | CiNL | CiQCL | CiSP | CiTAB)
/* lowercase a..z */
#define C_LOWER  CiLOWER
/* not alnux or dollar — separator for motion */
#define C_MFS    (CiALIAS | CiANGLE | CiBRACK | CiCNTRL | CiCOLON | CiCR | \
                  CiCURLY | CiEQUAL | CiGRAVE | CiHASH | CiMINUS | CiNL | \
                  CiNUL | CiPERCT | CiPLUS | CiQC | CiQCL | CiQCM | CiQCX | \
                  CiQUEST | CiSP | CiSPX | CiTAB)
/* octal digit 0..7 */
#define C_OCTAL  CiOCTAL
/* pattern magical operator chars (no space) */
#define C_PATMO  (CiPLUS | CiQUEST | CiVAR1)
/* POSIX printable chars */
#define C_PRINT  (C_GRAPH | CiSP)
/* POSIX punctuation */
#define C_PUNCT  (CiALIAS | CiANGLE | CiBRACK | CiCOLON | CiCURLY | CiEQUAL | \
                  CiGRAVE | CiHASH | CiMINUS | CiPERCT | CiPLUS | CiQC | \
                  CiQCL | CiQCM | CiQCX | CiQUEST | CiSS | CiUNDER)
/* chars requiring quoting, minus space */
#define C_QUOTE  (CiANGLE | CiBRACK | CiCURLY | CiEQUAL | CiGRAVE | CiHASH | \
                  CiNL | CiQC | CiQCL | CiQCX | CiQUEST | CiSS | CiTAB)
/* hex digit */
#define C_SEDEC  (CiDIGIT | CiHEXLT | CiOCTAL)
/* POSIX whitespace */
#define C_SPACE  (CiCR | CiNL | CiSP | CiSPX | CiTAB)
/* substitution operations with word: + - = ? */
#define C_SUB1   (CiEQUAL | CiMINUS | CiPLUS | CiQUEST)
/* substitution operations with pattern: # % */
#define C_SUB2   (CiHASH | CiPERCT)
/* uppercase A..Z */
#define C_UPPER  CiUPPER
/* substitution parameters other than positional */
#define C_VAR1   (CiHASH | CiMINUS | CiQUEST | CiSS | CiVAR1)

/* Individual character shorthands */
#define C_ANGLE  CiANGLE
#define C_COLON  CiCOLON
#define C_CR     CiCR
#define C_DOLAR  CiSS
#define C_EQUAL  CiEQUAL
#define C_GRAVE  CiGRAVE
#define C_HASH   CiHASH
#define C_LF     CiNL
#define C_MINUS  CiMINUS
#define C_NL     CiNL
#define C_NUL    CiNUL
#define C_PLUS   CiPLUS
#define C_QC     CiQC
#define C_QUEST  CiQUEST
#define C_SPC    CiSP
#define C_TAB    CiTAB
#define C_UNDER  CiUNDER

/* -----------------------------------------------------------------
 * Tables.  Filled by misc.c at startup; qsh_ctypes[c] yields the
 * 30-bit mask for byte c.
 * ----------------------------------------------------------------- */

EXTERN k32 qsh_ctypes[256];
EXTERN const char digits_uc[] E_INIT("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ");
EXTERN const char digits_lc[] E_INIT("0123456789abcdefghijklmnopqrstuvwxyz");

/* -----------------------------------------------------------------
 * Test helpers.  ord(c) is in sh.h (KBI(c)).
 * ----------------------------------------------------------------- */

#define ORD(c)              ord(c)              /* may evaluate twice */
#define HAS(v, f)           (((v) & (f)) == (f))
#define asciibetical(c)     KUI(ord(c))
#define rtt2asc(c)          KBY(c)
#define asc2rtt(c)          KBY(c)

/* case-independent / case-sensitive single-char compares */
#define isCh(c, u, l)       ((ord(c) | 0x20U) == ord(l))
#define isch(c, t)          (ord(c) == ORD(t))

/* control character */
#define qsh_isctrl(c)       ((ord(c) & 0x7FU) < 0x20U || ord(c) == 0x7FU)
#define qsh_asisctrl(c)     (ord(c) < 0x20U || ord(c) == 0x7FU)
/* Flag(FASIS) — set -o asis: don't strip the high bit */
#define qsh_isctrl8(c)      (Flag(FASIS) ? qsh_asisctrl(c) : qsh_isctrl(c))

/* membership test using the table */
#define ctype(c, t)         ((bool)(qsh_ctypes[ord(c)] & (t)))
#define cinttype(c, t)      ((c) >= 0 && (c) <= 0xFF \
                             ? ((bool)(qsh_ctypes[KBY(c)] & (t))) : false)

/* dash detector: s == "-" exactly */
#define qsh_isdash(s)       ((bool)(ord((s)[0]) == '-' && ord((s)[1]) == '\0'))

/* case fold (works through the table, not <ctype.h>) */
#define qsh_tolower(c)      (ctype(c, C_UPPER) ? (c) - 'A' + 'a' : (c))
#define qsh_toupper(c)      (ctype(c, C_LOWER) ? (c) - 'a' + 'A' : (c))

/* single-digit value */
#define qsh_numdig(c)       (ord(c) - ORD('0'))
#define qsh_numuc(c)        (rtt2asc(c) - rtt2asc('A'))
#define qsh_numlc(c)        (rtt2asc(c) - rtt2asc('a'))

#endif /* _QRV_SH_CCLASS_H */
