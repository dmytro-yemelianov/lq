/*
 * syscall.h — QSOE stub.
 *
 * The upstream musl `src/internal/syscall.h` defines `__syscall`,
 * `syscall_cp`, and the architecture's inline-asm bridge to ecall.
 * We excluded it (and all its callers) when we tore out the
 * __syscall hackery in v0.7.
 *
 * A few musl-internal headers we DO keep — most notably
 * `src/internal/stdio_impl.h` — still do `#include "syscall.h"`
 * unconditionally.  This file satisfies that include for QSOE-side
 * code (under userland/libc/qsoe/) so we can pull stdio_impl.h's
 * FILE struct definition into our os_dependent translation units
 * without having to redeclare the layout ourselves.
 *
 * Intentionally empty: any os_dependent file that needs an actual
 * syscall calls libqsoe directly (qsoe_close, qsoe_write, etc.).
 */
#ifndef _QSOE_SYSCALL_H
#define _QSOE_SYSCALL_H
#endif
