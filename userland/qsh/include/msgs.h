/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pool of message strings used throughout the shell.  Extracted
 * out of sh.h, where it lived alongside ~3000 lines of unrelated
 * declarations.  Strings are shared via offset arithmetic — many
 * Txxx names are #define'd as `(Tlonger_string + N)` to point at
 * a substring of a longer string and save .rodata.
 *
 * This file relies on EXTERN and E_INIT(i) being defined by the
 * caller (sh.h takes care of that just before it #include's us).
 */

/* Helpers for the offset-pool aliases below */
#define Tnl (Tf_s_s_sN + 8)
#define T1space (Treal_sp2 + 5)
#define TC_IFSWS (TinitIFS + 4)

EXTERN const char TinitIFS[] E_INIT("IFS= \t\n");
EXTERN const char TFCEDIT_dollaru[] E_INIT("${FCEDIT:-/bin/ed} \"$_\"");
#define Tspdollaru (TFCEDIT_dollaru + 18)
EXTERN const char Tsgdot[] E_INIT("*=.");
EXTERN const char Taugo[] E_INIT("augo");
EXTERN const char Tbracket[] E_INIT("[");
#define Tdot (Tsgdot + 2)
#define Talias (Tunalias + 2)
EXTERN const char Tbadnum[] E_INIT("bad number");
#define Tbadsubst (Tfg_badsubst + 10)
EXTERN const char Tbg[] E_INIT("bg");
EXTERN const char Tbad_buf[] E_INIT("%s: buf %zX len %zd");
EXTERN const char Tbad_flags[] E_INIT("%s: flags 0x%08X");
EXTERN const char Tbad_sig[] E_INIT("bad signal");
EXTERN const char Tsgbreak[] E_INIT("*=break");
#define Tbreak (Tsgbreak + 2)
EXTERN const char T__builtin[] E_INIT("-\\builtin");
#define T_builtin (T__builtin + 1)
#define Tbuiltin (T__builtin + 2)
EXTERN const char Tcant_cd[] E_INIT("restricted shell - can't cd");
EXTERN const char Tcant_filesub[] E_INIT("can't open $(<...) file");
#define Tcd (Tcant_cd + 25)
EXTERN const char Tcloexec_failed[] E_INIT("failed to %s close-on-exec flag for fd#%d");
#define T_command (T_funny_command + 9)
#define Tcommand (Trderr + 14)
EXTERN const char Tsgcontinue[] E_INIT("*=continue");
#define Tcontinue (Tsgcontinue + 2)
EXTERN const char Tcreate[] E_INIT("create");
EXTERN const char TDone[] E_INIT("Done (%d)");
EXTERN const char TELIF_unexpected[] E_INIT("TELIF unexpected");
EXTERN const char TEXECSHELL[] E_INIT("EXECSHELL");
EXTERN const char TENV[] E_INIT("ENV");
EXTERN const char Tdsgexport[] E_INIT("^*=export");
#define Texport (Tdsgexport + 3)
EXTERN const char Tfalse[] E_INIT("false");
EXTERN const char Tfg[] E_INIT("fg");
EXTERN const char Tfg_badsubst[] E_INIT("fileglob: bad substitution");
#define Tfile (Tcant_filesub + 19)
EXTERN const char Tyankfirst[] E_INIT("\nyank something first");
#define Tfirst (Tyankfirst + 16)
EXTERN const char TFPATH[] E_INIT("FPATH");
EXTERN const char T_function[] E_INIT(" function");
#define Tfunction (T_function + 1)
EXTERN const char T_funny_command[] E_INIT("funny $()-command");
EXTERN const char Tgetopts[] E_INIT("getopts");
#define Tgetrusage (Ttime_getrusage + 6)
EXTERN const char Ttime_getrusage[] E_INIT("time: getrusage");
#define Thistory (Tnot_in_history + 7)
EXTERN const char Tintovfl[] E_INIT("integer overflow %zu %c %zu prevented");
EXTERN const char Tinvname[] E_INIT("%s: invalid %s name");
EXTERN const char Tjobs[] E_INIT("jobs");
EXTERN const char Tjob_not_started[] E_INIT("job not started");
EXTERN const char Tqsh[] E_INIT("qsh");
#define Tname (Tinvname + 15)
EXTERN const char Tnil[] E_INIT("(null)");
EXTERN const char Tno_args[] E_INIT("missing argument");
EXTERN const char Tno_OLDPWD[] E_INIT("no OLDPWD");
EXTERN const char Tnot_ident[] E_INIT("not an identifier");
EXTERN const char Tnot_in_history[] E_INIT("not in history");
#define Tnot_found (Tinacc_not_found + 16)
#define Tsp_not_found (Tinacc_not_found + 15)
EXTERN const char Tinacc_not_found[] E_INIT("inaccessible or not found");
#define Tnot_started (Tjob_not_started + 4)
#define TOLDPWD (Tno_OLDPWD + 3)
EXTERN const char Topen[] E_INIT("open");
EXTERN const char Ttooearly[] E_INIT("too early%s");
EXTERN const char To_o_reset[] E_INIT(" -o .reset");
#define To_reset (To_o_reset + 4)
#define TPATH (TFPATH + 1)
#define Tpo (T_set_po + 5)
#define Tpv (TpVv + 1)
EXTERN const char TpVv[] E_INIT("Vpv");
#define TPWD (Tno_OLDPWD + 6)
EXTERN const char Trderr[] E_INIT("while reading command");
#define Tread (Tshf_read + 4)
EXTERN const char Tdsgreadonly[] E_INIT("^*=readonly");
#define Treadonly (Tdsgreadonly + 3)
EXTERN const char Tread_only[] E_INIT("read-only");
EXTERN const char Tredirection_dup[] E_INIT("can't finish (dup) redirection");
#define Tredirection (Tredirection_dup + 19)
#define Treal_sp1 (Treal_sp2 + 1)
EXTERN const char Treal_sp2[] E_INIT(" real ");
EXTERN const char TREPLY[] E_INIT("REPLY");
EXTERN const char Treq_arg[] E_INIT("requires an argument");
EXTERN const char Tselect[] E_INIT("select");
#define Tset (Tf_parm + 14)
#define Tset_po (T_set_po + 1)
EXTERN const char T_set_po[] E_INIT(" set +o");
EXTERN const char Tsghset[] E_INIT("*=#set");
#define Tsh (Tqsh + 1)
#define TSHELL (TEXECSHELL + 4)
EXTERN const char Tshell[] E_INIT("shell");
EXTERN const char Tshf_read[] E_INIT("shf_read");
EXTERN const char Tshf_write[] E_INIT("shf_write");
EXTERN const char Tgsource[] E_INIT("=source");
#define Tsource (Tgsource + 1)
EXTERN const char Tj_suspend[] E_INIT("j_suspend");
#define Tsuspend (Tj_suspend + 2)
EXTERN const char Tsynerr[] E_INIT("syntax error");
EXTERN const char Ttime[] E_INIT("time");
EXTERN const char Ttoo_many_args[] E_INIT("too many arguments");
EXTERN const char Ttoo_many_files[] E_INIT("too many open files (%d -> %d)");
EXTERN const char Ttrue[] E_INIT("true");
EXTERN const char Tdgtypeset[] E_INIT("^=typeset");
#define Ttypeset (Tdgtypeset + 2)
#define Tugo (Taugo + 1)
EXTERN const char Tunalias[] E_INIT("unalias");
#define Tunexpected (TELIF_unexpected + 6)
EXTERN const char Tunexpected_type[] E_INIT("%s: unexpected %s type %d");
EXTERN const char Tunknown_option[] E_INIT("unknown option");
EXTERN const char Tunwind[] E_INIT("unwind");
#define Tuser_sp1 (Tuser_sp2 + 1)
EXTERN const char Tuser_sp2[] E_INIT(" user ");
#define Twrite (Tshf_write + 4)
#define Thex32 (Tbad_flags + 12)
EXTERN const char Tf__S[] E_INIT(" %S");
#define Tf__d (Tunexpected_type + 22)
#define Tf__sN (Tf_s_s_sN + 5)
EXTERN const char Tf_s_T[] E_INIT("%s %T");
#define Tf_T (Tf_s_T + 3)
EXTERN const char Tf_dN[] E_INIT("%d\n");
EXTERN const char Tf_s_[] E_INIT("%s ");
EXTERN const char Tf_s_s_sN[] E_INIT("%s %s %s\n");
#define Tf__s_s (Tf_sD_s_s + 3)
EXTERN const char Tf_s_sD_s[] E_INIT("%s %s: %s");
EXTERN const char Tf_parm[] E_INIT("parameter not set");
EXTERN const char Tf_cant_s[] E_INIT("%s: can't %s");
EXTERN const char Tf_heredoc[] E_INIT("here document '%s' unclosed");
EXTERN const char Tf_S_[] E_INIT("%S ");
#define Tf_S (Tf__S + 1)
EXTERN const char Tf_sSQlu[] E_INIT("%s[%lu]");
#define Tf_SQlu (Tf_sSQlu + 2)
#define Tf_lu (Tf_toolarge + 14)
EXTERN const char Tf_toolarge[] E_INIT("%s too large: %lu");
EXTERN const char Tf_ldfailed[] E_INIT("%s tcsetpgrp(%d, %ld)");
EXTERN const char Tf_toomany[] E_INIT("too many %ss");
EXTERN const char Tf_sd[] E_INIT("%s %d");
#define Tf_s (Tf_temp + 24)
EXTERN const char Tft_end[] E_INIT("%;");
#define Tft_R (Tft_s_R + 3)
EXTERN const char Tft_s_R[] E_INIT("%s %R");
#define Tf_d (Tcloexec_failed + 39)
EXTERN const char Tf_sD_s_qs[] E_INIT("%s: %s '%s'");
EXTERN const char Tf_temp[] E_INIT("can't %s temporary file %s");
EXTERN const char Tf_ssfailed[] E_INIT("%s: %s failed");
EXTERN const char Tf__c_[] E_INIT("-%c ");
EXTERN const char Tf_sD_s_s[] E_INIT("%s: %s %s");
#define Tf_sN (Tf_s_s_sN + 6)
#define Tf_sD_s (Tf_s_sD_s + 3)
