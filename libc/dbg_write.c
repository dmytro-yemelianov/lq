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
 * The ecall shape mirrors taskman/sel4_syscalls.h; replicated here
 * because the libc seam doesn't include taskman headers.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/qsoe.h>

/* seL4 RISC-V syscall ABI: a7 = syscall number, a0 = argument.
 * SysDebugPutChar is -9 under CONFIG_PRINTING -- keep in step with
 * taskman/sel4_syscalls.h. */
#define LQ_SEL4_SYS_DEBUG_PUTCHAR  (-9)

void qsoe_dbg_write(const char *buf, unsigned long len)
{
    for (unsigned long i = 0; i < len; i++) {
        register long _a0 __asm__("a0") = (long)(unsigned char)buf[i];
        register long _a7 __asm__("a7") = LQ_SEL4_SYS_DEBUG_PUTCHAR;
        __asm__ volatile ("ecall"
                          : "+r"(_a0)
                          : "r"(_a7)
                          : "memory");
    }
}
