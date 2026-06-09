/*
 * sel4_syscalls.h — minimal seL4 syscall wrappers for taskman.
 *
 * For now we only need DebugPutChar (a debug-build kernel facility that
 * prints a byte to the kernel console via ecall). As QSOE grows toward a
 * proper QNX-style libc and replacement for libsel4, this header will
 * expand with the real IPC syscalls (Send, Recv, Call, ReplyRecv, ...).
 *
 * RISC-V seL4 syscall ABI:
 *   a7 = syscall number   (SysDebugPutChar = -12 under MCS+CONFIG_PRINTING)
 *   a0 = first argument
 *   ecall
 */
#ifndef QSOE_SEL4_SYSCALLS_H
#define QSOE_SEL4_SYSCALLS_H

/* The debug-build console putchar syscall number is config-dependent:
 * it sits just past the IPC syscalls, so enabling MCS (which adds
 * Wait/NBWait) shifts it (-9 non-MCS -> -12 under MCS; -9 is now
 * SysWait!).  Derive it from the kernel's generated enum instead of
 * hardcoding — sel4_types.h pulls in <arch/api/syscall.h>. */
#include "sel4_types.h"
#define SEL4_SYS_DEBUG_PUTCHAR ((long)SysDebugPutChar)

/* SchedYield's syscall number, same story: it sits among the IPC
 * syscalls, so MCS's inserted Wait/NBWait/NBSendWait/NBSendRecv shift
 * it (stock -7 -> -11 under MCS, where -7 is now SysRecv).  Derive it
 * from the generated enum so the LQ libc seam (qsoe/timer.c) can't
 * drift from the kernel ABI. */
#define SEL4_SYS_YIELD ((long)SysYield)

static inline void sel4_debug_putchar(char c)
{
    register long _a0 __asm__("a0") = (long)(unsigned char)c;
    register long _a7 __asm__("a7") = SEL4_SYS_DEBUG_PUTCHAR;
    __asm__ volatile("ecall"
                 : "+r"(_a0)
                 : "r"(_a7)
                 : "memory", "a1", "a2", "a3", "a4", "a5", "a6");
}

static inline void sel4_debug_puts(const char *s)
{
    while (*s) {
        sel4_debug_putchar(*s++);
    }
}

#endif /* QSOE_SEL4_SYSCALLS_H */
