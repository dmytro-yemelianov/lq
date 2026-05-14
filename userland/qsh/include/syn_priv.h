/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Private header for the parser pieces split across:
 *   syn.c           — early parser (pipeline / andor / c_list /
 *                     synio / nested / get_command) + utilities
 *                     (initkeywords, newtp, syntaxerr, nesting_*)
 *   syn_compound.c  — compound command parsers (dogroup, thenpart,
 *                     elsepart, caselist, casepart, wordlist, block,
 *                     function_body, compile) + the top-level
 *                     yyparse driver
 *   error.c         — yyerror / kerrf* / kwarnf* / merrF / bi_*
 *
 * Functions and state visible to more than one of these files have
 * their declarations here; the corresponding definitions are no
 * longer `static`.
 */
#ifndef QSH_SYN_PRIV_H
#define QSH_SYN_PRIV_H

#include "sh.h"

/* Parser-internal types shared between syn.c and syn_compound.c. */
struct nesting_state {
    int start_token; /* token that began nesting (eg, FOR) */
    int start_line;  /* line nesting began on */
};

struct yyrecursive_state {
    struct ioword *old_heres[HERES];
    struct yyrecursive_state *next;
    struct ioword **old_herep;
    int old_symbol;
    unsigned int old_nesting_type;
    bool old_reject;
};

/* parser entry points + helpers from syn.c */
struct op *pipeline(int cf, int sALIAS);
struct op *andor(int sALIAS);
struct op *c_list(int sALIAS, bool multi);
struct ioword *synio(int cf);
struct op *nested(int type, unsigned int smark, unsigned int emark, int sALIAS);
struct op *get_command(int cf, int sALIAS);
int inalias(struct source *);
void syntaxerr(const char *) QSH_A_NORETURN;
void nesting_push(struct nesting_state *, int);
void nesting_pop(struct nesting_state *);
struct op *newtp(int);

/* parser entry points + helpers from syn_compound.c */
void yyparse(bool doalias);
struct op *dogroup(int sALIAS);
struct op *thenpart(int sALIAS);
struct op *elsepart(int sALIAS);
struct op *caselist(int sALIAS);
struct op *casepart(int endtok, int sALIAS);
struct op *function_body(char *, int, bool);
char **wordlist(int);
struct op *block(int, struct op *, struct op *);

/* error funcs from error.c */
void vwarnf(unsigned int, int, const char *, va_list);
void vwarnf0(unsigned int, int, const char *, va_list) QSH_A_FORMAT(__printf__, 3, 0);

/* shared state owned by syn.c */
extern struct op *outtree;           /* yyparse output */
extern struct nesting_state nesting; /* \n changed to ; */
extern bool reject;                  /* token(cf) gets symbol again */
extern int symbol;                   /* yylex value */
extern const char Tcbrace[];
extern const char Tesac[];

/* token-acceptance helpers shared between the parser pieces */
#define REJECT (reject = true)
#define ACCEPT (reject = false)
#define token(cf) ((reject ? 0 : (symbol = yylex(cf))), ACCEPT, symbol)
#define tpeek(cf) ((reject ? 0 : (symbol = yylex(cf))), REJECT, symbol)
#define musthave(c, cf)                                                                            \
    do {                                                                                           \
        if ((unsigned int)token(cf) != (unsigned int)(c))                                          \
            syntaxerr(NULL);                                                                       \
    } while (/* CONSTCOND */ 0)

#endif /* QSH_SYN_PRIV_H */
