/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Built-in command tables and the smallest helpers.  The actual
 * implementations live in funcs_io.c, funcs_alias.c, funcs_jobs.c,
 * funcs_shctl.c, funcs_read.c, funcs_time.c, funcs_test.c.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/* tiny local builtins */
static int c_true(const char **);
static int c_false(const char **);

/* getn() that prints error — shared across funcs_*.c */
int
bi_getn(const char *as, int *ai)
{
    int rv;

    if (!(rv = getn(as, ai)))
        bi_errorf(Tf_sD_s, Tbadnum, as);
    return (rv);
}

static int
c_true(const char **wp QSH_A_UNUSED)
{
    return (0);
}

static int
c_false(const char **wp QSH_A_UNUSED)
{
    return (1);
}

/*
 * A leading = means assignments before command are kept.
 * A leading * means a POSIX special builtin.
 * A leading ^ means declaration utility, - declaration forwarder.
 * A leading ~ means external utilities override this, ! with flags only.
 * A leading # means is set or shift (for argc/argv bookkeeping).
 */
const struct builtin qshbuiltins[] = {
    {Tsgdot, c_dot},
    {"*=:", c_true},
    {Tbracket, c_test},
    /* no =: AT&T manual wrong */
    {Talias, c_alias},
    {Tsgbreak, c_brkcont},
    {T__builtin, c_builtin},
    {Tbuiltin, c_builtin},
    {Tcd, c_cd},
    /* dash compatibility hack */
    {"chdir", c_cd},
    {T_command, c_command},
    {Tsgcontinue, c_brkcont},
    {"echo", c_print},
    {"*=eval", c_eval},
    {"*=exec", c_exec},
    {"*=exit", c_exitreturn},
    {Tdsgexport, c_typeset},
    {Tfalse, c_false},
    {"fc", c_fc},
    {Tgetopts, c_getopts},
    {"id", c_id},
    {Tjobs, c_jobs},
    {"kill", c_kill},
    {"let", c_let},
    {"print", c_print},
    {"pwd", c_pwd},
    {Tread, c_read},
    {Tdsgreadonly, c_typeset},
    {"!realpath", c_realpath},
    {"*=return", c_exitreturn},
    {Tsghset, c_set},
    {"*=#shift", c_shift},
    {Tgsource, c_dot},
    {"test", c_test},
    /* normally a syntax element but as_builtin or 'x=y time foo' do: */
    {Ttime, do_evalcmd},
    {"*=times", c_times},
    {"*=trap", c_trap},
    {Ttrue, c_true},
    {Tdgtypeset, c_typeset},
    {"ulimit", c_ulimit},
    {"umask", c_umask},
    {Tunalias, c_unalias},
    {"*=unset", c_unset},
    {"wait", c_wait},
    {"whence", c_whence},
    /* QRV: no `bind` builtin — line editor has no configurable keymap. */
    {NULL, (int (*)(const char **))NULL}};

const struct t_op u_ops[] = {
    /* 0*/ {"-a", TO_FILAXST}, {"-b", TO_FILBDEV},
    /* 2*/ {"-c", TO_FILCDEV}, {"-d", TO_FILID},   {"-e", TO_FILEXST}, {"-f", TO_FILREG},
    {"-G", TO_FILGID},         {"-g", TO_FILSETG}, {"-H", TO_FILCDF},  {"-h", TO_FILSYM},
    {"-k", TO_FILSTCK},        {"-L", TO_FILSYM},
    /*12*/ {"-n", TO_STNZE},   {"-O", TO_FILUID},
    /*14*/ {"-o", TO_OPTION},
    /*15*/ {"-p", TO_FILFIFO},
    /*16*/ {"-r", TO_FILRD},   {"-S", TO_FILSOCK}, {"-s", TO_FILGZ},   {"-t", TO_FILTT},
    /*20*/ {"-u", TO_FILSETU}, {"-v", TO_ISSET},   {"-w", TO_FILWR},
    /*23*/ {"-x", TO_FILEX},   {"-z", TO_STZER},   {"", TO_NONOP}};
qCTA_BEG(funcs_c);
qCTA(u_ops_size, NELEM(u_ops) == 26);
qCTA_END(funcs_c);
const struct t_op b_ops[] = {
    {"=", TO_STEQL},   {"==", TO_STEQL},  {"!=", TO_STNEQ},  {"<", TO_STLT},    {">", TO_STGT},
    {"-eq", TO_INTEQ}, {"-ne", TO_INTNE}, {"-gt", TO_INTGT}, {"-ge", TO_INTGE}, {"-lt", TO_INTLT},
    {"-le", TO_INTLE}, {"-ef", TO_FILEQ}, {"-nt", TO_FILNT}, {"-ot", TO_FILOT}, {"", TO_NONOP}};
