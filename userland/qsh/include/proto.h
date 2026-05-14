/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Function prototypes from each .c file, plus the small types
 * that didn't naturally land in any of the other topical headers
 * (Trap, Temp_type, Test_op/Test_env).  Pulled in from
 * sh.h so callers don't need to remember which file owns which
 * extern.
 */

#ifndef _QRV_SH_PROTO_H
#define _QRV_SH_PROTO_H

#include "sh.h"     /* foundation: Area, bool, k32, qsh_ari_t, etc. */

/* Forward references — these structs live in topical headers. */
struct op;
struct ioword;
struct shf;
struct tbl;
struct table;
struct tstate;
struct columnise_opts;

/* -----------------------------------------------------------------
 * Trap (signal handler) record + flags.  Implementation in
 * histrap.c.  Replaced by libc's own signal table in the long run
 * (see doc/md/libc_signals_two_thread.md), but for the shell's
 * own bookkeeping the Trap struct still tracks user-facing trap
 * commands, restoration policy, etc.
 * ----------------------------------------------------------------- */

#define qsh_NSIG        _NSIG
#define qsh_SIGEXIT     0
#define qsh_SIGERR      qsh_NSIG

typedef struct trap {
    const char           *name;     /* short name */
    const char           *mess;     /* descriptive name */
    char                 *trap;     /* trap command */
    sig_t                 cursig;   /* current handler */
    sig_t                 shtrap;   /* shell handler */
    int                   signal;   /* signal number */
    int                   flags;    /* TF_* below */
    volatile sig_atomic_t set;      /* trap pending */
} Trap;

#define TF_SHELL_USES   BIT(0)      /* shell uses signal, user can't change */
#define TF_USER_SET     BIT(1)      /* user has tried to set trap */
#define TF_ORIG_IGN     BIT(2)      /* original action was SIG_IGN */
#define TF_ORIG_DFL     BIT(3)      /* original action was SIG_DFL */
#define TF_EXEC_IGN     BIT(4)      /* restore SIG_IGN before exec */
#define TF_EXEC_DFL     BIT(5)      /* restore SIG_DFL before exec */
#define TF_DFL_INTR     BIT(6)      /* default action is LINTR */
#define TF_TTY_INTR     BIT(7)      /* tty-generated signal */
#define TF_CHANGED      BIT(8)      /* runtrap() change detector */
#define TF_FATAL        BIT(9)      /* terminates if not trapped */

/* setsig() / setexecsig() flag bits */
#define SS_RESTORE_MASK 0x3
#define SS_RESTORE_CURR 0
#define SS_RESTORE_ORIG 1
#define SS_RESTORE_DFL  2
#define SS_RESTORE_IGN  3
#define SS_FORCE        BIT(3)      /* override original SIG_IGN */
#define SS_USER         BIT(4)      /* trap command */
#define SS_SHTRAP       BIT(5)      /* internal-use trap */

EXTERN volatile sig_atomic_t trap;
EXTERN volatile sig_atomic_t intrsig;
EXTERN volatile sig_atomic_t fatal_trap;
extern Trap sigtraps[qsh_NSIG + 1];
EXTERN volatile sig_atomic_t got_winch E_INIT(1);

enum tmout_enum {
    TMOUT_EXECUTING = 0,
    TMOUT_READING,
    TMOUT_LEAVING
};
EXTERN enum tmout_enum qsh_tmout_state;

/* -----------------------------------------------------------------
 * Temp files (here-docs, FUNSUB, fc -e).  Owned by the current
 * env; freed when the env is unwound.
 * ----------------------------------------------------------------- */

typedef kby Temp_type;
#define TT_HEREDOC_EXP  0
#define TT_HIST_EDIT    1
#define TT_FUNSUB       2

struct temp {
    struct temp *next;
    struct shf  *shf;
    pid_t        pid;               /* process that parsed the heredoc */
    Temp_type    type;
    qccFAMslot(char, tffn);
};

/* -----------------------------------------------------------------
 * test / [[ ]] / [ ] machinery.  Implementation in funcs.c
 * (c_test) + funcs.c (the dbtestp_* / ptest_* functions).
 * ----------------------------------------------------------------- */

enum Test_op {
    TO_NONOP = 0,
    /* unary */
    TO_STNZE, TO_STZER, TO_ISSET, TO_OPTION,
    TO_FILAXST, TO_FILEXST, TO_FILREG, TO_FILBDEV, TO_FILCDEV,
    TO_FILSYM, TO_FILFIFO, TO_FILSOCK, TO_FILCDF, TO_FILID,
    TO_FILGID, TO_FILSETG, TO_FILSTCK, TO_FILUID, TO_FILRD,
    TO_FILGZ, TO_FILTT, TO_FILSETU, TO_FILWR, TO_FILEX,
    /* binary */
    TO_STEQL, TO_STNEQ, TO_STLT, TO_STGT,
    TO_INTEQ, TO_INTNE, TO_INTGT, TO_INTGE, TO_INTLT, TO_INTLE,
    TO_FILEQ, TO_FILNT, TO_FILOT,
    TO_NONNULL  /* sentinel — !TO_NONOP */
};
typedef enum Test_op Test_op;

enum Test_meta {
    TM_OR, TM_AND, TM_NOT, TM_OPAREN, TM_CPAREN,
    TM_UNOP, TM_BINOP, TM_END
};
typedef enum Test_meta Test_meta;

struct t_op {
    const char  op_text[4];
    Test_op     op_num;
};
extern const struct t_op u_ops[];
extern const struct t_op b_ops[];

/* shorthand strings for some single-char operators */
#define Tda (u_ops[0].op_text)
#define Tdc (u_ops[2].op_text)
#define Tdn (u_ops[12].op_text)
#define Tdo (u_ops[14].op_text)
#define Tdp (u_ops[15].op_text)
#define Tdr (u_ops[16].op_text)
#define Tdu (u_ops[20].op_text)
#define Tdx (u_ops[23].op_text)
#define Tu  (Tdu + 1)

#define TEF_ERROR    BIT(0)
#define TEF_DBRACKET BIT(1)

typedef struct test_env {
    union {
        const char **wp;            /* ptest_* */
        XPtrV       *av;            /* dbtestp_* */
    } pos;
    const char **wp_end;            /* ptest_* */
    Test_op    (*isa)(struct test_env *, Test_meta);
    const char *(*getopnd)(struct test_env *, Test_op, bool);
    int        (*eval)(struct test_env *, Test_op, const char *, const char *, bool);
    void       (*error)(struct test_env *, int, const char *);
    int          flags;
} Test_env;

extern const char *const dbtest_tokens[];

Test_op test_isop(Test_meta, const char *);
int     test_eval(Test_env *, Test_op, const char *, const char *, bool);
int     test_parse(Test_env *);

/* -----------------------------------------------------------------
 * tty state.  tty_fd is the file descriptor of /dev/tty (or stdin
 * if tty_devtty is false).
 * ----------------------------------------------------------------- */

EXTERN int        tty_fd E_INIT(-1);
EXTERN bool       tty_devtty;

extern int tty_init_fd(void);

/* -----------------------------------------------------------------
 * Path helpers (POSIX-only, no DOS/OS2 separators).
 * ----------------------------------------------------------------- */

#define qsh_abspath(s)         (ord((s)[0]) == ORD('/'))
#define qsh_cdirsep(c)         (ord(c) == ORD('/'))
#define qsh_sdirsep(s)         ucstrchr((s), '/')
#define qsh_vdirsep(s)         vstrchr((s), '/')

/* Used by str_val() and friends to decide what to do on error */
#define QSH_UNWIND_ERROR  0     /* unwind the stack (qshlongjmp) */
#define QSH_RETURN_ERROR  1     /* return 1/0 for success/failure */

/* aresizeif — grow only if the new size really needs it */
#define aresizeif(z, p, n, ap)                                                              \
    (((p) == NULL) || ((z) < (n)) || (((z) & ~X_WASTE) > ((n) & ~X_WASTE))                  \
         ? aresize((p), (n), (ap))                                                          \
         : (p))

/* -----------------------------------------------------------------
 * Globals carried over from the original sh.h.
 * ----------------------------------------------------------------- */

EXTERN const char *qshname;             /* $0 */
EXTERN const char *safe_prompt;
EXTERN const char *builtin_argv0;
EXTERN char       *current_wd;
EXTERN char       *path;                /* copy of either PATH or def_path */
EXTERN const char *def_path;
EXTERN char       *tmpdir;
EXTERN const char *prompt;
EXTERN bool        shl_stdout_ok;
EXTERN char      **history;
EXTERN char      **histptr;

/* "QSH_VERSION=@(#)MIRBSD KSH ..."  — initial value of the
   $QSH_VERSION variable.  Defined in main.c. */
EXTERN const char initvsn[] E_INIT("QSH_VERSION=@(#)qsh " QSH_VERSION);
#define QSH_VERSION         (initvsn + 16)
EXTERN qsh_ari_t  histsize;            /* history size */
EXTERN qsh_ari_t  x_cols E_INIT(80);   /* terminal width */
EXTERN qsh_ari_t  x_lins E_INIT(24);   /* terminal height */
EXTERN int         current_lineno;      /* LINENO value */
EXTERN unsigned int qsh_tmout;          /* TMOUT timer */
EXTERN kby          cur_prompt;         /* PS1 or PS2 active */
EXTERN bool        builtin_spec;        /* called builtin is POSIX-special */
EXTERN sigset_t    sm_default;          /* default signal mask */
EXTERN sigset_t    sm_sigchld;          /* mask blocking SIGCHLD */

/* coproc state — single coprocess per shell */
typedef unsigned int Coproc_id;
struct coproc {
    void     *job;
    int       read;     /* coproc -> us */
    int       readw;    /* other side of read (saved temporarily) */
    int       write;    /* us -> coproc */
    int       njobs;
    Coproc_id id;
};
EXTERN struct coproc coproc;

/* getrusage wrapper — implemented in misc.c (calls getrusage on systems
   that have it; computes from /proc otherwise).  QRV will need its own
   implementation. */
extern int qsh_getrusage(int, struct rusage *);

/* job-accounting cumulative timers, set by jobs.c */
EXTERN struct timeval j_usrtime;
EXTERN struct timeval j_systime;

/* exit-on-next-quit flag set by the EXIT trap and friends */
EXTERN bool really_exit;

/* heredoc accumulation during parsing — heres[] is a stack of pending
   ioword pointers, herep is the next free slot. */
EXTERN struct ioword *heres[HERES];
EXTERN struct ioword **herep;

#define IDENT 64
EXTERN char ident[IDENT + 1];

/* -----------------------------------------------------------------
 * edit.c
 * ----------------------------------------------------------------- */

/* Line editor — small in-house implementation in edit.c. */
void  x_init(void);
char *x_read(char *);
void  x_initterm(const char *);

/* -----------------------------------------------------------------
 * eval.c
 * ----------------------------------------------------------------- */

char  *substitute(const char *, int);
char **eval(const char **, int);
char  *evalstr(const char *, int);
char  *evalonestr(const char *, int);
char  *debunk(char *, const char *, size_t);
void   expand(const char *, XPtrV *, int);
int    glob_str(char *, XPtrV *, bool);
char  *do_tilde(char *);

/* -----------------------------------------------------------------
 * exec.c
 * ----------------------------------------------------------------- */

int          execute(struct op *volatile, volatile int, volatile int *volatile);
int          c_builtin(const char **);
struct tbl  *get_builtin(const char *);
struct tbl  *findfunc(const char *, k32, bool);
int          define(const char *, struct op *);
const char  *builtin(const char *, int (*)(const char **));
struct tbl  *findcom(const char *, int);
void         flushcom(bool);
int          search_access(const char *, int);
const char  *search_path(const char *, const char *, int, int *);
void         pr_menu(const char *const *);
void         pr_list(struct columnise_opts *, char *const *);
int          herein(struct ioword *, char **);
const char **cpyargv(int *, const char **, Area *);

/* -----------------------------------------------------------------
 * expr.c (arithmetic), plus utf-8 helpers from misc.c.
 * ----------------------------------------------------------------- */

int  evaluate(const char *, qsh_ari_t *, int, bool);
int  v_evaluate(struct tbl *, const char *, volatile int, bool);

char  *ez_bs(char *, char *);
size_t utf_mbtowc(unsigned int *, const char *);
size_t utf_wctomb(char *, unsigned int);
#define OPTUISRAW(wc)   IS(wc, 0xFFFFFF80U, 0x0000EF80U)
#define OPTUMKRAW(ch)   (ord(ch) | 0x0000EF00U)
#define ez_mbtowc       ez_mbtoc
size_t ez_mbtoc(unsigned int *, const char *);
size_t ez_ctomb(char *, unsigned int);
int    utf_wcwidth(unsigned int);
int    qsh_access(const char *, int);
struct tbl *tempvar(const char *);

/* -----------------------------------------------------------------
 * funcs.c — builtin-command entry points.
 * ----------------------------------------------------------------- */

int   c_hash(const char **);
int   c_pwd(const char **);
int   c_print(const char **);
int   c_id(const char **);
int   c_whence(const char **);
int   c_command(const char **);
int   c_typeset(const char **);
bool  valid_alias_name(const char *);
int   c_alias(const char **);
int   c_unalias(const char **);
int   c_let(const char **);
int   c_jobs(const char **);
int   c_kill(const char **);
void  getopts_reset(int);
int   c_getopts(const char **);
int   c_bind(const char **);
int   c_shift(const char **);
int   c_umask(const char **);
int   c_dot(const char **);
int   c_wait(const char **);
int   c_read(const char **);
int   c_eval(const char **);
int   do_evalcmd(const char **);
int   bi_getn(const char *, int *);
int   c_trap(const char **);
int   c_brkcont(const char **);
int   c_exitreturn(const char **);
int   c_set(const char **);
int   c_unset(const char **);
int   c_ulimit(const char **);
int   c_times(const char **);
int   timex(struct op *, int, volatile int *);
void  timex_hook(struct op *, char **volatile *);
int   c_exec(const char **);
int   c_test(const char **);
int   c_realpath(const char **);
int   c_rename(const char **);

/* -----------------------------------------------------------------
 * histrap.c
 * ----------------------------------------------------------------- */

void  init_histvec(void);
void  hist_init(Source *);
void  histsave(int *, const char *, int, bool);
bool  histsync(void);
int   c_fc(const char **);
void  sethistsize(qsh_ari_t);
char **histpos(void);
int   histnum(int);
int   findhist(int, const char *, bool, bool);
char **hist_get_newest(bool);
void  inittraps(void);
void  alarm_init(void);
Trap *gettrap(const char *, bool, bool);
void  trapsig(int);
void  intrcheck(void);
int   fatal_trap_check(void);
int   trap_pending(void);
void  runtraps(int);
void  runtrap(Trap *, bool);
void  cleartraps(void);
void  restoresigs(void);
void  settrap(Trap *, const char *);
bool  block_pipe(void);
void  restore_pipe(void);
int   setsig(Trap *, sig_t, int);
void  setexecsig(Trap *, int);
void  qsh_lockfd(int);
void  qsh_unlkfd(int);

/* -----------------------------------------------------------------
 * jobs.c — process management (fork-free path on QRV).
 * ----------------------------------------------------------------- */

void  j_init(void);
void  j_exit(void);
int   exchild(struct op *, int, volatile int *, int);
void  startlast(void);
int   waitlast(void);
int   waitfor(const char *, int *);
int   j_kill(const char *, int);
void  j_suspend(void);
int   j_jobs(const char *, int, int);
void  j_notify(void);
pid_t j_async(void);
int   j_stopped_running(void);

/* -----------------------------------------------------------------
 * lex.c — extras not in lex.h
 * ----------------------------------------------------------------- */

void  set_prompt(int, Source *);
int   pprompt(const char *, int);

/* -----------------------------------------------------------------
 * main.c
 * ----------------------------------------------------------------- */

kby   qshname_islogin(const char **);
int   include(const char *, const char **, bool);
int   command(const char *, int);
int   shell(Source *volatile, volatile int);
void  unwind(int) QSH_A_NORETURN;
void  newenv(int);
void  quitenv(struct shf *);
void  cleanup_parents_env(void);
void  cleanup_proc_env(void);
void  bi_errorf(const char *, ...) QSH_A_FORMAT(__printf__, 1, 2);
void  shellf(const char *, ...)    QSH_A_FORMAT(__printf__, 1, 2);
void  shprintf(const char *, ...)  QSH_A_FORMAT(__printf__, 1, 2);
int   can_seek(int);
void  initio(void);
int   qsh_dup2(int, int, bool);
int   savefd(int);
void  restfd(int, int);
void  openpipe(int *);
void  closepipe(int *);
int   check_fd(const char *, int, const char **);
void  coproc_init(void);
void  coproc_read_close(int);
void  coproc_readw_close(int);
void  coproc_write_close(int);
int   coproc_getfd(int, const char **);
void  coproc_cleanup(int);
struct temp *maketemp(Area *, Temp_type, struct temp **);

/* extra hash-table interfaces (the basics live in var.h) */
struct tbl **ktsort(struct table *);

const char *qsh_getwd(void);

/* -----------------------------------------------------------------
 * misc.c — option parsing, glob, getopt, misc.
 * ----------------------------------------------------------------- */

size_t option(const char *);
char  *getoptions(void);
void   change_flag(enum sh_flag, unsigned int, bool);
void   change_xtrace(unsigned char, bool);
int    parse_args(const char **, unsigned int, bool *);
int    getn(const char *, int *);
int    getnh(const char *, qiHUGE_U *);
int    gmatchx(const char *, const char *, bool);
bool   has_globbing(const char *);
int    ascstrcmp(const void *, const void *);
int    ascpstrcmp(const void *, const void *);
void   qsh_getopt_opterr(int, const char *, const char *);
void   qsh_getopt_reset(Getopt *, int);
int    qsh_getopt(const char **, Getopt *, const char *);
char  *quote_value(const char *);
void   print_columns(struct columnise_opts *, unsigned int,
                     void (*)(char *, size_t, unsigned int, const void *),
                     const void *, size_t, size_t);
void   strip_nuls(char *, size_t)
                  QSH_A_BOUNDED(__string__, 1, 2);
int    reset_nonblock(int);
char  *do_realpath(const char *);
void   simplify_path(char *);
void   set_current_wd(const char *);
int    c_cd(const char **);
int    unbksl(bool, int (*)(void), void (*)(int));
/* ucstrchr / vstrchr are defined as macros in sh.h foundation. */

/* -----------------------------------------------------------------
 * shf.c — set_ifs lives here next to the IFS state.
 * ----------------------------------------------------------------- */

void   set_ifs(const char *);

/* -----------------------------------------------------------------
 * syn.c
 * ----------------------------------------------------------------- */

bool   parse_usec(const char *, struct timeval *);
char  *yyrecursive(int);
void   yyerror(const char *, ...) QSH_A_NORETURN QSH_A_FORMAT(__printf__, 1, 2);

/* error / warning — see also sh.h foundation */
void   bi_unwind(int);
bool   error_prefix(bool);
void   kwarnf(unsigned int, ...);
void   kwarnf1(unsigned int, int, const char *, ...) QSH_A_FORMAT(__printf__, 3, 4);
void   kerrf1(unsigned int, int, const char *, ...) QSH_A_NORETURN
                                                    QSH_A_FORMAT(__printf__, 3, 4);
void   merrF(int *, unsigned int, ...);
#define merrf(rv, va)                                                                       \
    do {                                                                                    \
        merrF va;                                                                           \
        return (rv);                                                                        \
    } while (/* CONSTCOND */ 0)

/* -----------------------------------------------------------------
 * tree.c
 * ----------------------------------------------------------------- */

void        fptreef(struct shf *, int, const char *, ...);
char       *snptreef(char *, ssize_t, const char *, ...);
const char *wdscan(const char *, int);
#define WDS_TPUTS  BIT(0)               /* tputS (dumpwdvar) mode */
char       *wdstrip(const char *, int);
void        uprntc(unsigned char, struct shf *);
void        uescmbT(unsigned char *, const char **)
                    QSH_A_BOUNDED(__minbytes__, 1, 5);
int         uwidthmbT(char *, char **);
const char *uprntmbs(const char *, bool, struct shf *);
void        fpFUNCTf(struct shf *, int, bool, const char *, struct op *);

/* -----------------------------------------------------------------
 * var.c — extras beyond what var.h declares.
 * ----------------------------------------------------------------- */

void        newblock(void);
void        popblock(void);
void        initvar(void);
struct block *varsearch(struct block *, struct tbl **, const char *, k32);
struct tbl *global(const char *);
struct tbl *isglobal(const char *, bool);
struct tbl *local(const char *, bool);
char       *str_val(struct tbl *);
int         setstr(struct tbl *, const char *, int);
struct tbl *setint_v(struct tbl *, struct tbl *, bool);
void        setint(struct tbl *, qsh_ari_t);
void        setint_n(struct tbl *, qsh_ari_t, int);
struct tbl *typeset(const char *, kui, kui, int, int);
void        unset(struct tbl *, int);
const char *skip_varname(const char *, bool);
const char *skip_wdvarname(const char *, bool);
int         is_wdvarname(const char *, bool);
int         is_wdvarassign(const char *, bool);
struct tbl *arraysearch(struct tbl *, k32);
char      **makenv(void);
void        change_winsz(void);
size_t      array_ref_len(const char *);
struct tbl *arraybase(const char *);
qsh_uari_t set_array(const char *, bool, const char **);
k32         hash(const void *);
void        chvt_rndsetup(const void *, size_t);
k32         rndget(void);
void        rndset(unsigned long);
void        rndpush(const void *, size_t);
void        record_match(const char *);

#endif /* _QRV_SH_PROTO_H */
