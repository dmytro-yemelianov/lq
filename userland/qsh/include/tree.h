/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Parser AST: command-tree node (`struct op`), tree-type tags
 * (TCOM, TPIPE, TFOR, ...), redirection record (`struct ioword`),
 * the small set of expansion-flag and exchild-flag bits used
 * during execution, and the word-prefix codes that flag bytes
 * inside a word string (CHAR, QCHAR, OQUOTE, etc.).
 *
 * Logically lives between the parser (syn.c) and the executor
 * (exec.c, eval.c) — both touch every field declared here.
 */

#ifndef _QRV_SH_TREE_H
#define _QRV_SH_TREE_H

#include "sh.h"     /* Area, kby, BIT(), bool */

struct ioword;      /* forward — defined below */

/*
 * Description of a command or an operation on commands.
 * One node of the parsed AST.
 */
struct op {
    const char    **args;       /* arguments to a command */
    char         **vars;        /* variable assignments */
    struct ioword **ioact;      /* I/O actions (e.g. < > >>) */
    struct op     *left, *right;
    char          *str;         /* word for case; identifier for for / select /
                                   functions; path to execute for TEXEC; time
                                   hook for TCOM */
    int            lineno;      /* TCOM/TFUNC: LINENO for this node */
    short          type;        /* operation type — see T*-tags below */
    union {
        short evalflags;        /* TCOM: arg-expansion eval() flags */
        short qsh_func;         /* TFUNC: function x  vs  x() */
        char  charflag;         /* TPAT: termination character */
    } u;
};

/* op.type values */
#define TEOF        0
#define TCOM        1   /* command */
#define TPAREN      2   /* (c-list) */
#define TPIPE       3   /* a | b */
#define TLIST       4   /* a ; b */
#define TOR         5   /* || */
#define TAND        6   /* && */
#define TBANG       7   /* ! */
#define TDBRACKET   8   /* [[ .. ]] */
#define TFOR        9
#define TSELECT     10
#define TCASE       11
#define TIF         12
#define TWHILE      13
#define TUNTIL      14
#define TELIF       15
#define TPAT        16  /* pattern in case */
#define TBRACE      17  /* {c-list} */
#define TASYNC      18  /* c & */
#define TFUNCT      19  /* function name { command; } */
#define TTIME       20  /* time pipeline */
#define TEXEC       21  /* posix_spawn-eval'd TCOM */
#define TCOPROC     22  /* coprocess |& */

/*
 * Word-prefix codes — single bytes that mark the start of a token
 * piece inside a word string returned by the lexer.
 */
#define EOS         0   /* end of string */
#define CHAR        1   /* unquoted character */
#define QCHAR       2   /* quoted character */
#define COMSUB      3   /* $() substitution (NUL-terminated) */
#define EXPRSUB     4   /* $(()) substitution (NUL-terminated) */
#define OQUOTE      5   /* opening " or ' */
#define CQUOTE      6   /* closing " or ' */
#define OSUBST      7   /* opening ${ subst (followed by { or X) */
#define CSUBST      8   /* closing } of above */
#define OPAT        9   /* open pattern: *(, @(, etc. */
#define SPAT        10  /* separator: | */
#define CPAT        11  /* close pattern: ) */
#define ADELIM      12  /* arbitrary delimiter: ${foo:2:3}, ${foo/bar/baz} */
#define FUNSUB      14  /* ${ foo;} substitution */
#define VALSUB      15  /* ${|foo;} substitution */
#define COMASUB     16  /* `…` substitution (COMSUB but expand aliases) */
#define FUNASUB     17  /* function substitution + alias expansion */

/*
 * I/O redirection record.
 */
struct ioword {
    char           *ioname;     /* filename (unused if heredoc) */
    char           *delim;      /* delimiter for << / <<- */
    char           *heredoc;    /* content of heredoc */
    unsigned short  ioflag;     /* action (IO* below) */
    signed char     unit;       /* unit (fd) affected */
};

/* ioword.ioflag — type of redirection */
#define IOTYPE      0xF         /* type: bits 0..3 */
#define IOREAD      0x1         /* < */
#define IOWRITE     0x2         /* > */
#define IORDWR      0x3         /* <> */
#define IOHERE      0x4         /* << (here file) */
#define IOCAT       0x5         /* >> */
#define IODUP       0x6         /* <& / >& */
#define IOEVAL      BIT(4)      /* expand in << */
#define IOSKIP      BIT(5)      /* <<-, skip leading tabs */
#define IOCLOB      BIT(6)      /* >| — override noclobber */
#define IORDUP      BIT(7)      /* x<&y (vs x>&y) */
#define IODUPSELF   BIT(8)      /* x>&x (vs x>&y) */
#define IONAMEXP    BIT(9)      /* name has been expanded */
#define IOBASH      BIT(10)     /* &> etc. */
#define IOHERESTR   BIT(11)     /* <<< (here string) */
#define IONDELIM    BIT(12)     /* null delimiter (<<) */
#define IOSYNIONEXT BIT(13)     /* already fully configured */

/*
 * exchild() flags — control how a TCOM is executed.
 * QRV substitutes posix_spawn for fork/exec; XFORK/XEXEC retain
 * their classic meanings as scheduling hints.
 */
#define XEXEC       BIT(0)      /* execute without spawning */
#define XFORK       BIT(1)      /* spawn before executing */
#define XBGND       BIT(2)      /* command & */
#define XPIPEI      BIT(3)      /* input is pipe */
#define XPIPEO      BIT(4)      /* output is pipe */
#define XXCOM       BIT(5)      /* `...` command */
#define XPCLOSE     BIT(6)      /* exchild: close fd in parent */
#define XCCLOSE     BIT(7)      /* exchild: close fd in child */
#define XERROK      BIT(8)      /* non-zero exit ok (for set -e) */
#define XCOPROC     BIT(9)      /* starting a co-process */
#define XTIME       BIT(10)     /* timing TCOM command */
#define XPIPEST     BIT(11)     /* want PIPESTATUS */

/*
 * Expansion-control flags — fit in struct op.u.evalflags (short).
 */
#define DOBLANK         BIT(0)  /* perform blank interpretation */
#define DOGLOB          BIT(1)  /* expand [?* */
#define DOPAT           BIT(2)  /* quote *?[ */
#define DOTILDE         BIT(3)  /* normal ~ expansion (first char) */
#define DONTRUNCOMMAND  BIT(4)  /* do not run $(command) things */
#define DOASNTILDE      BIT(5)  /* assignment ~ expansion (after =, :) */
#define DOBRACE         BIT(6)  /* expand(): do brace expansion */
#define DOMAGIC         BIT(7)  /* expand(): string contains MAGIC */
#define DOTEMP          BIT(8)  /* dito: in word part of ${..[%#=?]..} */
#define DOVACHECK       BIT(9)  /* var-assign check (typeset/set/...) */
#define DOMARKDIRS      BIT(10) /* force markdirs behaviour */
#define DOTCOMEXEC      BIT(11) /* sh -c hack — not really an eval flag */
#define DOSCALAR        BIT(12) /* non-list field handling */
#define DOHEREDOC       BIT(13) /* scalar handling for heredoc body */
#define DOHERESTR       BIT(14) /* append a newline char */
#define DODBMAGIC       BIT(15) /* add magic for [[ x = $y ]] */

/* AST manipulation API (implemented in tree.c).  Print/format helpers
   that take a struct shf are declared in proto.h once shf is in scope. */
struct op  *tcopy(struct op *, Area *);
char       *wdcopy(const char *, Area *);
void        tfree(struct op *, Area *);

#endif /* _QRV_SH_TREE_H */
