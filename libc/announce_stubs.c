/*
 * announce_stubs.c -- LQ libc stubs for symbols that libc.so imports
 *                     but doesn't yet have a real implementation for.
 *
 * Per `feedback_stubs_announce`: every stub MUST emit a
 *   STUB: <name> returning <value>
 * line on first call.  Silent stubs get forgotten and waste hours
 * later.  Each function here uses ANNOUNCE_ONCE() to print exactly
 * once per process, then returns a sensible no-op value.
 *
 * Symbols stubbed here are surfaced by taskman's reloc walker at
 * load time -- it emits a "[WARN] reloc skip" line per missing
 * symbol.  Adding a definition here moves that symbol from "silently
 * NULL" to "announcing stub", and the load-time WARN goes away
 * because taskman's pre-reloc now resolves it.
 *
 * Not stubbed here (intentionally):
 *   - main          : the user image's entry; comes from qsh.
 *   - __environ     : now real in the shared libc (libc/qsoe/environ.c),
 *                     with environ weak-aliased to it.
 *   - tolower & the ctype family : now real in the shared libc
 *                     (libc/ctype/ctype.c).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <wctype.h>
#include <sys/types.h>
#include <sys/qsoe.h>           /* Sync*, Thread* declarations */
#define qsoe_errno_h_already_pulled_in_via_qsoe_h 1
extern int *__errno_location(void);
#define errno (*__errno_location())
#ifndef ENOSYS
# define ENOSYS 89
#endif

#define ANNOUNCE_ONCE(name, ret_desc) do {                                 \
    static int announced;                                                  \
    if (!announced) {                                                      \
        announced = 1;                                                     \
        fprintf(stderr, "STUB: " name " returning " ret_desc "\n");        \
    }                                                                      \
} while (0)

/* __environ now lives in the shared libc (libc/qsoe/environ.c), which
 * defines it as a valid empty environment and weak-aliases `environ` to
 * it; defining it here too would multiply-define the symbol. */

/* --- POSIX file-descriptor + tty stubs ----------------------------- */

/* ioctl() lives in the shared libc body (libc/qsoe/ioctl.c) --
 * termios fake-success + announcing fallthrough for both kernels.
 * tcdrain() is now real in the shared libc (libc/1/tcdrain.c). */

/* --- process / signal stubs ---------------------------------------- */

int execve(const char *path, char *const argv[], char *const envp[])
{
    ANNOUNCE_ONCE("execve", "-1 (ENOSYS)");
    (void)path; (void)argv; (void)envp;
    errno = ENOSYS;
    return -1;
}

/* raise() lives in the shared libc body (libc/qsoe/posix_stubs.c)
 * since 2026-06-05 -- one announcing stub for both kernels. */

/* --- stdio variants ------------------------------------------------ */

/* dprintf() / vdprintf() are now real in the shared libc
 * (libc/stdio/dprintf.c) -- they write straight to the fd.
 * vsprintf() likewise became real + shared (libc/stdio/vsprintf.c);
 * leaving it LQ-only here left NQ's crypt() calling a NULL vsprintf. */

/* tolower / the byte ctype family, the wide-char family (towlower,
 * towupper, iswctype, wctype) and mbtowc are now REAL in the shared libc:
 * libc/ctype/ (musl Unicode classification + case) and libc/multibyte/
 * (UTF-8).  __nl_langinfo_l is real in libc/qsoe/nl_langinfo.c (C/English
 * locale).  All formerly stubbed here. */

/* --- Sync* reentrant variants -------------------------------------- */
/* The non-_r versions exist in libc.so; some _r reentrant variants
 * aren't wired yet.  Forward each to its non-reentrant counterpart and
 * announce.  Declarations come from <sys/qsoe.h> included at the top.
 * (SyncCondvarWait_r / SyncCondvarSignal_r are now real in qsoe/sync.c.) */

long SyncDestroy_r(sync_t *s)
{
    ANNOUNCE_ONCE("SyncDestroy_r", "via SyncDestroy");
    return SyncDestroy(s);
}

/* --- threads ------------------------------------------------------- */

long ThreadDetach_r(int tid)
{
    ANNOUNCE_ONCE("ThreadDetach_r", "via ThreadDetach");
    return ThreadDetach(tid);
}
