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

/* --- process / signal stubs ---------------------------------------- */

int execve(const char *path, char *const argv[], char *const envp[])
{
    ANNOUNCE_ONCE("execve", "-1 (ENOSYS)");
    (void)path; (void)argv; (void)envp;
    errno = ENOSYS;
    return -1;
}
