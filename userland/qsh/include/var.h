/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shell variables, functions, aliases, builtins — all stored in
 * one polymorphic struct (`struct tbl`) hashed into struct table
 * instances.  A struct block is the activation record for a
 * function call or sourced script: it pins a fresh allocation
 * area, the local variable + function tables, and the arg vector.
 *
 * Types implemented in var.c; tree-walking helpers (ktinit,
 * ktsearch, ktenter, ktwalk, ktnext, ktdelete) implemented there
 * too.
 */

#ifndef _QRV_SH_VAR_H
#define _QRV_SH_VAR_H

#include "sh.h"     /* Area, kby, k32, kui, qsh_ari_t, qsh_uari_t,
                       BIT, EXTERN, qccFAMslot, qccFAMSZ */

struct op;          /* tree.h — function body for FUNC entries */

/* -----------------------------------------------------------------
 * struct table — hash bucket for tbls.  Power-of-two sized; tshift
 * is log2(buckets).  The table grows by ktwalk-style rehashing.
 * ----------------------------------------------------------------- */

struct table {
    Area        *areap;     /* area to allocate entries from */
    struct tbl **tbls;      /* hashed table items */
    size_t       nfree;     /* free entries before rehash */
    kby          tshift;    /* log2(buckets) */
};

/* -----------------------------------------------------------------
 * struct tbl — one variable / function / alias / builtin / keyword.
 * The tagged union is decoded by the .type and .flag fields.
 * ----------------------------------------------------------------- */

struct tbl {
    Area *areap;
    union {
        char           *s;          /* string */
        qsh_ari_t      i;          /* signed integer */
        qsh_uari_t     u;          /* unsigned integer */
        int           (*f)(const char **);  /* builtin entry point */
        struct op      *t;          /* function body */
    } val;
    union {
        struct tbl     *array;      /* array values — chain of tbls */
        const char     *fpath;      /* path of undef autoload function */
    } u;
    union {
        k32             hval;       /* hash(name) for normal vars */
        k32             index;      /* index for array elements */
    } ua;
    union {
        int             field;      /* width for typeset -L/-R/-Z */
        int             errnov;     /* CEXEC/CTALIAS errno */
    } u2;
    /*
     * type — see C* command-type constants below.  Also stores
     * the integer base when flag&INTEGER, or the offset of value
     * past the name= prefix when flag&EXPORT.
     */
    int      type;
    kui      flag;                  /* see flag bits below */

    /* qsh_int flexible-array trick — actual name stored inline */
    qccFAMslot(char, name);
};

/* sized initialiser for static struct tbl instances */
union tbl_static {
    struct tbl tbl;
    char       storage[qccFAMSZ(struct tbl, name, 4)];
};

/* set by isglobal(), global(), local() */
EXTERN bool last_lookup_was_array;
EXTERN struct tbl *vtemp;

/* -----------------------------------------------------------------
 * tbl.flag bits.
 *
 * Bits 0..7 are used by every tbl kind ("common").
 * Bits 8+ have different meanings depending on whether the entry
 * is a variable or a function/alias/builtin/keyword.
 * ----------------------------------------------------------------- */

/* common */
#define ALLOC       BIT(0)      /* val.s has been alloc'd, owns memory */
#define DEFINED     BIT(1)      /* defined in this block */
#define ISSET       BIT(2)      /* val.[siu] populated */
#define EXPORT      BIT(3)      /* exported variable / function */
#define TRACE       BIT(4)      /* var: user-flagged; func: -x trace */

/* variable-only (start at 8) */
#define SPECIAL     BIT(8)      /* PATH, IFS, SECONDS, etc */
#define INTEGER     BIT(9)      /* val.i contains integer value */
#define RDONLY      BIT(10)     /* read-only */
#define LOCAL       BIT(11)     /* local typeset() */
#define ARRAY       BIT(13)     /* array */
#define LJUST       BIT(14)     /* left-justify */
#define RJUST       BIT(15)     /* right-justify */
#define ZEROFIL     BIT(16)     /* RJUSTIFY: zero-fill; LJUSTIFY: strip 0s */
#define LCASEV      BIT(17)     /* lower-case */
#define UCASEV_AL   BIT(18)     /* upper-case / autoload */
#define INT_U       BIT(19)     /* unsigned integer */
#define INT_L       BIT(20)     /* long integer (no-op marker) */
#define IMPORT      BIT(21)     /* typeset(): no arrays, must have = */
#define LOCAL_COPY  BIT(22)     /* with LOCAL: copy attrs from existing */
#define EXPRINEVAL  BIT(23)     /* contents currently being evaluated */
#define EXPRLVALUE  BIT(24)     /* useable as lvalue (temp flag) */
#define AINDEX      BIT(25)     /* array index >0; ua.index valid */
#define ASSOC       BIT(26)     /* ARRAY ? associative : reference */

/* function/alias/builtin/keyword (also starting at 8) */
#define KEEPASN     BIT(8)      /* keep command assignments (var=x cmd) */
#define FINUSE      BIT(9)      /* function being executed */
#define FDELETE     BIT(10)     /* function deleted while executing */
#define FKSH        BIT(11)     /* defined with `function x` (vs x()) */
#define SPEC_BI     BIT(12)     /* POSIX special builtin */
#define LOWER_BI    BIT(13)     /* with LOW_BI: override even w/o flags */
#define LOW_BI      BIT(14)     /* external utility overrides builtin */
#define DECL_UTIL   BIT(15)     /* declaration utility */
#define DECL_FWDR   BIT(16)     /* declaration utility forwarder */
#define NEXTLOC_BI  BIT(17)     /* needs BF_RESETSPEC on e->loc */

/*
 * Attributes the user can set; used to decide whether an unset
 * param should be reported by set/typeset.  Excludes ARRAY/LOCAL.
 */
#define USERATTRIB  (EXPORT | INTEGER | RDONLY | LJUST | RJUST | ZEROFIL | \
                     LCASEV | UCASEV_AL | INT_U | INT_L)

#define arrayindex(vp) \
    ((unsigned long)((vp)->flag & AINDEX ? (vp)->ua.index : 0))

enum namerefflag { SRF_NOP, SRF_ENABLE, SRF_DISABLE };

/* -----------------------------------------------------------------
 * tbl.type — command/symbol category.
 * ----------------------------------------------------------------- */

#define CNONE       0   /* undefined */
#define CSHELL      1   /* built-in */
#define CFUNC       2   /* function */
#define CEXEC       4   /* executable command */
#define CALIAS      5   /* alias */
#define CKEYWD      6   /* keyword */
#define CTALIAS     7   /* tracked alias */

/* Flags for findcom() / comexec() */
#define FC_SPECBI   BIT(0)              /* special builtin */
#define FC_FUNC     BIT(1)              /* function */
#define FC_NORMBI   BIT(2)              /* not a special builtin */
#define FC_BI       (FC_SPECBI | FC_NORMBI)
#define FC_PATH     BIT(3)              /* do path search */
#define FC_DEFPATH  BIT(4)              /* use default path */
#define FC_WHENCE   BIT(5)              /* called by command/whence */

/* -----------------------------------------------------------------
 * Argument vector for $#, $* across function calls / sourced files.
 * ----------------------------------------------------------------- */

#define AF_ARGV_ALLOC   0x1   /* argv[] array allocated */
#define AF_ARGS_ALLOCED 0x2   /* argument strings allocated */
#define AI_ARGV(a, i)   ((i) == 0 ? (a).argv[0] : (a).argv[(i) - (a).skip])
#define AI_ARGC(a)      ((a).ai_argc - (a).skip)

struct arg_info {
    const char **argv;
    int          flags;
    int          ai_argc;
    int          skip;
};

/* -----------------------------------------------------------------
 * Block — the activation record for a function call / sourced
 * script / etc.  Forms a stack via the .next field.
 * ----------------------------------------------------------------- */

struct block {
    Area          area;         /* allocation area */
    const char  **argv;
    char         *error;        /* error handler */
    char         *exit;         /* exit handler */
    struct block *next;         /* enclosing block */
    struct table  vars;
    struct table  funs;
    Getopt        getopts_state;
    int           argc;
    int           flags;        /* BF_* */
};

/* block.flags */
#define BF_DOGETOPTS    BIT(0)          /* save/restore getopts state */
#define BF_STOPENV      BIT(1)          /* do not export further */
#define BF_RESETSPEC    BIT(17)         /* must equal NEXTLOC_BI */

/* -----------------------------------------------------------------
 * Hash-table walker state used by ktwalk()/ktnext().
 * ----------------------------------------------------------------- */

struct tstate {
    struct tbl **next;
    ssize_t      left;
};

/* -----------------------------------------------------------------
 * Globals — the four process-wide tables, plus the builtin list.
 * ----------------------------------------------------------------- */

EXTERN struct table taliases;       /* tracked aliases */
EXTERN struct table builtins;       /* built-in commands */
EXTERN struct table aliases;        /* aliases */
EXTERN struct table keywords;       /* keywords */

EXTERN struct tbl *vp_pipest;       /* PIPESTATUS array */

struct builtin {
    const char *name;
    int       (*func)(const char **);
};
extern const struct builtin qshbuiltins[];

/* -----------------------------------------------------------------
 * Hash-table API.  ktsearch and ktdelete are macros over the more
 * general primitives ktscan / direct flag-clear, kept for grep-
 * compatibility with upstream call sites.
 * ----------------------------------------------------------------- */

void        ktinit(Area *, struct table *, kby);
struct tbl *ktscan(struct table *, const char *, k32, struct tbl ***);
#define     ktsearch(tp, s, h)      ktscan((tp), (s), (h), NULL)
struct tbl *ktenter(struct table *, const char *, k32);
#define     ktdelete(p)             do { (p)->flag = 0; } while (0)
void        ktwalk(struct tstate *, struct table *);
struct tbl *ktnext(struct tstate *);

#endif /* _QRV_SH_VAR_H */
