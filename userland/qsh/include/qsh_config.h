/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * Adaptation for QRV: Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef _MKSH_QRV_CONFIG_H
#define _MKSH_QRV_CONFIG_H

/* ---------- Compiler attributes (gcc + clang both fine on RISC-V) ---- */
#define HAVE_ATTRIBUTE_BOUNDED 0
#define HAVE_ATTRIBUTE_FORMAT 1
#define HAVE_ATTRIBUTE_NORETURN 1
#define HAVE_ATTRIBUTE_UNUSED 1
#define HAVE_ATTRIBUTE_USED 1

/* ---------- Headers ------------------------------------------------- */
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME_H 1
#define HAVE_BOTH_TIME_H 1
#define HAVE_SELECT_TIME_H 1
#define HAVE_SYS_BSDTYPES_H 0
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_FILE_H 0
#define HAVE_SYS_SYSMACROS_H 0
#define HAVE_SYS_MKDEV_H 0
#define HAVE_SYS_MMAN_H 1
#define HAVE_SYS_PTEM_H 0
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_BSTRING_H 0
#define HAVE_GRP_H 1
#define HAVE_IO_H 0
#define HAVE_LIBGEN_H 0   /* QRV's <libgen.h> has a `waitfor` that collides with mksh's */
#define HAVE_LIBUTIL_H 0
#define HAVE_PATHS_H 1
#define HAVE_STDINT_H 1
#define HAVE_STRINGS_H 1
#define HAVE_TERMIOS_H 1
#define HAVE_ULIMIT_H 0

/* ---------- Compiler / language features ---------------------------- */
#define HAVE_CAN_INTTYPES 1
#define HAVE_OFF_T 1
#define HAVE_SIG_T 1
#define HAVE_INTCONSTEXPR_RSIZE_MAX 0

/* ---------- Library functions --------------------------------------- */
#define HAVE_FLOCK 0
#define HAVE_FLOCK_DECL 0
#define HAVE_GETRANDOM 0
#define HAVE_GETRUSAGE 0
#define HAVE_GET_CURRENT_DIR_NAME 0
#define HAVE_GETSID 0
#define HAVE_GETTIMEOFDAY 1
#define HAVE_KILLPG 0
#define HAVE_LOCK_FCNTL 1
#define HAVE_MEMMOVE 1
#define HAVE_MKNOD 0
#define HAVE_NICE 0
#define HAVE_PERSISTENT_HISTORY 0
#define HAVE_POSIX_UTF8_LOCALE 0
#define HAVE_RENAME 0
#define HAVE_REVOKE 0
#define HAVE_REVOKE_DECL 0
#define HAVE_RLIMIT 0
#define HAVE_SELECT 1
#define HAVE_SETGROUPS 0
#define HAVE_SETLOCALE_LCALL 0
#define HAVE_SETRESUGID 0
#define HAVE_SIGABBREV_NP 0
#define HAVE_SIGACTION 1
#define HAVE_SIGDESCR_NP 0
#define HAVE_ST_MTIMENSEC 0
#define HAVE_STRERROR 1
#define HAVE_STRERRORDESC_NP 0
#define HAVE_STRING_POOLING 0
#define HAVE_STRLCPY 1
#define HAVE_STRSIGNAL 0
#define HAVE_STRSTR 1
#define HAVE_SYS_ERRLIST 0
#define HAVE_SYS_ERRLIST_DECL 0
#define HAVE_SYS_SIGLIST 0
#define HAVE_SYS_SIGLIST_DECL 0
#define HAVE_SYS_SIGNAME 0

/* ---------- mksh feature knobs -------------------------------------- */
#define QSH_ASSUME_UTF8 0
#define QSH_DISABLE_REVOKE_WARNING 1
#define QSH_NOPWNAM 1      /* no getpwnam: no ~user expansion */
#define QSH_UNEMPLOYED 1   /* no job control: no setpgid+killpg */
#define QSH__NO_SETEUGID 1 /* no setresuid/setregid */
#define QSH_NO_SIGSUSPEND 1
#define QSH_NO_SIGSETJMP 1 /* sigsetjmp on QRV is a macro */
#define QSH_DISABLE_TTY_WARNING 1

/* Default paths */
#define QSH_DEFAULT_EXECSHELL "/bin/sh"
#define QSH_DEFAULT_TMPDIR "/tmp"

#endif /* _MKSH_QRV_CONFIG_H */
