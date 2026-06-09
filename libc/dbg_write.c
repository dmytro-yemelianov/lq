/*
 * dbg_write.c: LQ seam body of qsoe_dbg_write().
 *
 * The raw byte sink behind qsoe_dbgprintf (<sys/qsoe.h> documents
 * the contract).  On LQ it loops seL4's SysDebugPutChar -- a
 * debug-kernel-build facility (CONFIG_PRINTING) that prints one byte
 * to the kernel console per ecall.  The seL4 debug ABI has no
 * string-write syscall: libsel4's own seL4_DebugPutString is the
 * same per-char loop (libsel4/arch_include/riscv/sel4/arch/
 * syscalls.h), so this is the canonical shape, not a shortcut.
 * Slow, but qsoe_dbgprintf is a diagnostic path, not a data path;
 * a production (non-debug) seL4 build turns these into no-ops at
 * the kernel boundary.
 *
 * The syscall number is sourced from the one central, enum-derived
 * definition (taskman/sel4_syscalls.h, on the seam's -I path) rather
 * than a local literal -- a hand-rolled copy here is exactly what
 * drifted from the kernel ABI across the MCS switch (stale -9 became
 * SysWait and cap-faulted).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/qsoe.h>
#include "sel4_syscalls.h"   /* SEL4_SYS_DEBUG_PUTCHAR, from the seL4 enum */

void qsoe_dbg_write(const char *buf, unsigned long len)
{
    for (unsigned long i = 0; i < len; i++) {
        register long _a0 __asm__("a0") = (long)(unsigned char)buf[i];
        register long _a7 __asm__("a7") = SEL4_SYS_DEBUG_PUTCHAR;
        __asm__ volatile ("ecall"
                          : "+r"(_a0)
                          : "r"(_a7)
                          : "memory");
    }
}
