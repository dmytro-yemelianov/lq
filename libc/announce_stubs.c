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
 *   - __environ     : data; defined as a NULL pointer alongside the
 *                     announcing function stubs below.
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

/* --- envp pointer -- libc body references this from getenv(),
 *     exec*(), etc.  Defined as NULL so empty-environment behaviour
 *     is correct; the dyn-link path doesn't fill it yet. */
char **__environ = 0;

/* --- POSIX file-descriptor + tty stubs ----------------------------- */

/* ioctl() lives in the shared libc body (libc/qsoe/ioctl.c) --
 * termios fake-success + announcing fallthrough for both kernels. */

int tcdrain(int fd)
{
    ANNOUNCE_ONCE("tcdrain", "0 (no-op)");
    (void)fd;
    return 0;
}

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

int dprintf(int fd, const char *fmt, ...)
{
    ANNOUNCE_ONCE("dprintf", "vfprintf-via-stderr");
    /* Best-effort: route through stderr so output isn't lost.  Real
     * dprintf would write to fd directly. */
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stderr, fmt, ap);
    va_end(ap);
    (void)fd;
    return n;
}

int vsprintf(char *str, const char *fmt, va_list ap)
{
    ANNOUNCE_ONCE("vsprintf", "via vsnprintf(8KiB cap)");
    /* No real buffer-size bound at this API; cap at 8 KiB and hope
     * the caller doesn't blow it.  Real vsprintf would inherit the
     * caller's stack. */
    return vsnprintf(str, 8192, fmt, ap);
}

/* --- wide-char / ctype stubs --------------------------------------- */

int tolower(int c)
{
    ANNOUNCE_ONCE("tolower", "ASCII fold");
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

wint_t towlower(wint_t wc)
{
    ANNOUNCE_ONCE("towlower", "ASCII fold (BMP-only)");
    if (wc >= L'A' && wc <= L'Z') return wc + (L'a' - L'A');
    return wc;
}

wint_t towupper(wint_t wc)
{
    ANNOUNCE_ONCE("towupper", "ASCII fold (BMP-only)");
    if (wc >= L'a' && wc <= L'z') return wc - (L'a' - L'A');
    return wc;
}

int iswctype(wint_t wc, wctype_t desc)
{
    ANNOUNCE_ONCE("iswctype", "0 (no classification)");
    (void)wc; (void)desc;
    return 0;
}

wctype_t wctype(const char *name)
{
    ANNOUNCE_ONCE("wctype", "0 (no class)");
    (void)name;
    return 0;
}

int mbtowc(wchar_t *pwc, const char *s, size_t n)
{
    ANNOUNCE_ONCE("mbtowc", "ASCII pass-through");
    (void)n;
    if (!s) return 0;
    if (pwc) *pwc = (wchar_t)(unsigned char)*s;
    return *s ? 1 : 0;
}

/* --- locale -------------------------------------------------------- */

#include <locale.h>

char *__nl_langinfo_l(int item, locale_t loc)
{
    ANNOUNCE_ONCE("__nl_langinfo_l", "\"\" (empty)");
    (void)item; (void)loc;
    return (char *)"";
}

/* --- Sync* reentrant variants -------------------------------------- */
/* The non-_r versions exist in libc.so; the _r reentrant variants
 * (which take an explicit errno location) aren't wired yet.  Forward
 * each to its non-reentrant counterpart and announce.  Declarations
 * come from <sys/qsoe.h> included at the top. */

long SyncCondvarSignal_r(sync_t *cv, int all)
{
    ANNOUNCE_ONCE("SyncCondvarSignal_r", "via SyncCondvarSignal");
    return SyncCondvarSignal(cv, all);
}

long SyncCondvarWait_r(sync_t *cv, sync_t *mx)
{
    ANNOUNCE_ONCE("SyncCondvarWait_r", "via SyncCondvarWait");
    return SyncCondvarWait(cv, mx);
}

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
