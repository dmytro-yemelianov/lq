/*
 * mprotect.c -- LQ stub for POSIX mprotect.
 *
 * rtld resolves mprotect from libc.so at startup to adjust GNU_RELRO
 * page permissions during relocation.  Until taskman grows a real
 * TM_REQ_MPROTECT path (variable-granularity seL4 Page_Map with
 * adjusted rights), this stub returns success without touching the
 * mapping.  Pages stay at whatever rights they were created with --
 * for v0 dynamic-linked binaries that means writable + readable on
 * the data segment, which is the correct "loose" posture: relocations
 * can be applied, RELRO simply doesn't take effect.  Real mprotect
 * lands when the security-hardening pass needs it.
 *
 * Announces itself on first call per the announcing-stubs policy --
 * silent stubs get forgotten and waste hours later (see
 * feedback_stubs_announce memory).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/mman.h>
#include <stdio.h>

int mprotect(void *addr, size_t len, int prot)
{
    static int announced;
    if (!announced) {
        announced = 1;
        fprintf(stderr, "STUB: mprotect returning 0 (no-op, RELRO inert)\n");
    }
    (void)addr; (void)len; (void)prot;
    return 0;
}
