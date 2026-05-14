/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Lexer / parser interface.  struct source is the input stack:
 * each script, function body, alias expansion, here-doc, or
 * `qsh -c "..."` argument pushes a new Source on the chain
 * rooted at `source`.  yyparse() pops tokens from the topmost
 * Source until S_EOF and returns a parsed AST (struct op).
 *
 * Implementation in lex.c (tokeniser) and syn.c (parser).
 */

#ifndef _QRV_SH_LEX_H
#define _QRV_SH_LEX_H

#include "sh.h"     /* Area, XString, BIT */
#include "tree.h"   /* struct op, struct ioword */

struct shf;
struct tbl;

typedef struct source Source;
struct source {
    XString      xs;            /* input buffer */
    Area        *areap;
    Source      *next;          /* stacked source */
    const char  *str;           /* read pointer */
    const char  *start;         /* buffer start */
    const char  *file;          /* input file name */
    union {
        const char **strv;      /* SWORDS */
        struct shf  *shf;       /* SFILE / SSTDIN */
        struct tbl  *tblp;      /* SALIAS — SF_HASALIAS */
        char        *freeme;    /* SREREAD — buffer to afree */
    } u;
    int          flags;         /* SF_* */
    int          type;          /* S* below */
    int          line;
    int          errline;       /* line of error (0 if none) */
    char         ugbuf[2];      /* ungetsc() / SALIAS / SREREAD */
};

/* source.type */
#define SEOF            0   /* input EOF */
#define SFILE           1   /* file input */
#define SSTDIN          2   /* read stdin */
#define SSTRING         3   /* string */
#define SWSTR           4   /* string without \n */
#define SWORDS          5   /* string[] */
#define SWORDSEP        6   /* string[] separator */
#define SALIAS          7   /* alias expansion */
#define SREREAD         8   /* read ahead to be re-scanned */
#define SSTRINGCMDLINE  9   /* string from "qsh -c ..." */

/* source.flags */
#define SF_ECHO         BIT(0)  /* echo input to shlout */
#define SF_ALIAS        BIT(1)  /* fake space at end of alias */
#define SF_ALIASEND     BIT(2)
#define SF_TTY          BIT(3)  /* SSTDIN and isatty() */
#define SF_HASALIAS     BIT(4)  /* u.tblp valid */
#define SF_MAYEXEC      BIT(5)  /* sh -c optimisation hack */

/* -----------------------------------------------------------------
 * Parser tokens — values returned by yylex().  LWORD upwards must
 * not collide with any single-character token (' ', '|', ';', etc.)
 * which yylex() returns directly.  Keep in sync with tokentab[]
 * in syn.c.
 * ----------------------------------------------------------------- */

typedef union {
    int             i;
    char           *cp;
    char          **wp;
    struct op      *o;
    struct ioword  *iop;
} YYSTYPE;

#define LWORD       256
#define LOGAND      257     /* && */
#define LOGOR       258     /* || */
#define BREAK       259     /* ;; */
#define IF          260
#define THEN        261
#define ELSE        262
#define ELIF        263
#define FI          264
#define CASE        265
#define ESAC        266
#define FOR         267
#define SELECT      268
#define WHILE       269
#define UNTIL       270
#define DO          271
#define DONE        272
#define IN          273
#define FUNCTION    274
#define TIME        275
#define REDIR       276
#define MDPAREN     277     /* (( )) */
#define BANG        278     /* ! */
#define DBRACKET    279     /* [[ .. ]] */
#define COPROC      280     /* coprocess marker */
#define BRKEV       281     /* ;& */
#define BRKFT       282     /* ;| */

/* -----------------------------------------------------------------
 * Lexer / parser API.
 * ----------------------------------------------------------------- */

EXTERN Source *source;          /* top of input stack */
EXTERN YYSTYPE yylval;          /* yylex() output */

Source *pushs(int, Area *);
void    yyskiputf8bom(void);
int     yylex(int);
struct op *compile(Source *, bool);
void    initkeywords(void);

/* parser-state flags returned by/passed to yylex() */
#define CONTIN          BIT(0)  /* skip newlines to complete command */
#define ALIAS           BIT(2)  /* recognise alias */
#define KEYWORD         BIT(3)  /* recognise keywords */
#define LETEXPR         BIT(4)  /* parse (( )) expression */
#define CMDASN          BIT(5)  /* parse x[1 & 2] as one word, typeset */
#define ESACONLY        BIT(7)  /* only accept esac keyword */
#define CMDWORD         BIT(8)  /* parsing simple command (alias) */
#define HEREDELIM       BIT(9)  /* parsing << / <<- delimiter */
#define LQCHAR          BIT(10) /* source string contains QCHAR */

/* default size of one input "line" buffer */
#define LINE            (16384 - ALLOC_OVERHEAD)

/* sretrace_info — full struct lives in lex.c; everyone else only
   needs the forward declaration env.h already provides. */

struct yyrecursive_state;       /* opaque to consumers — internal to syn.c */

/* helpers used by exec.c during $(...) expansion.  yyrecursive() saves
   parser state and starts a fresh parse; yyrecursive_pop(true) pops
   *all* nested states (used after a long-jump unwind). */
char *yyrecursive(int);
void  yyrecursive_pop(bool popall);

#endif /* _QRV_SH_LEX_H */
