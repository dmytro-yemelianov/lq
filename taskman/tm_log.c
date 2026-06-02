/*
 * tm_log.c — sole sel4_debug_* callsite in taskman.
 *
 * Implements the printf-lite formatter behind tm_err/warn/info/dbg/
 * trace, plus the raw byte-stream emit used by the /dev/console
 * resmgr's early-boot write path.
 *
 * Future work: when the trace-buffer subsystem lands, this file
 * grows a second emit path that writes each formatted message into
 * a ring buffer alongside the live putchar.  Callers don't change —
 * they keep calling tm_err(...) etc.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sel4_syscalls.h"
#include "tm_log.h"

/* va_list machinery — taskman builds with -nostdinc so reach for the
 * compiler builtins directly rather than <stdarg.h>. */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

/* ---- The one and only sel4_debug callsite ----------------------- */

static inline void put_char(char c) { sel4_debug_putchar(c); }
static inline void put_str (const char *s) { sel4_debug_puts(s); }

/* tm_raw_putc — see tm_log.h.  Inline-equivalent to put_char so the
 * /dev/console resmgr pays no overhead, but keeps the rule that
 * outside this file no caller mentions sel4_debug_*. */
void tm_raw_putc(char c) { put_char(c); }

/* ---- printf-lite ------------------------------------------------- */

static void emit_uhex(unsigned long v, int width, int zero_pad)
{
    char buf[16];
    int  n = 0;
    if (v == 0) buf[n++] = '0';
    else while (v) {
        unsigned d = v & 0xfu;
        buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
    if (zero_pad) {
        while (n < width) buf[n++] = '0';
    } else {
        while (n < width) put_char(' ');
    }
    while (n > 0) put_char(buf[--n]);
}

static void emit_udec(unsigned long v)
{
    char buf[24];
    int  n = 0;
    if (v == 0) buf[n++] = '0';
    else while (v) {
        buf[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) put_char(buf[--n]);
}

static void emit_sdec(long v)
{
    if (v < 0) {
        put_char('-');
        emit_udec((unsigned long)(-v));
    } else {
        emit_udec((unsigned long)v);
    }
}

static void vemit(const char *fmt, va_list ap)
{
    for (; *fmt; ++fmt) {
        if (*fmt != '%') { put_char(*fmt); continue; }
        ++fmt;
        if (*fmt == 0) break;

        int zero  = 0;
        int width = 0;
        if (*fmt == '0') { zero = 1; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }
        /* Accept and ignore `l` / `ll` — RV64 long IS 64 bits and
         * the variadic-arg promotions handle it uniformly. */
        while (*fmt == 'l') ++fmt;

        switch (*fmt) {
        case 's': put_str(va_arg(ap, const char *)); break;
        case 'c': put_char((char)va_arg(ap, int));    break;
        case 'd': emit_sdec(va_arg(ap, long));        break;
        case 'u': emit_udec(va_arg(ap, unsigned long));break;
        case 'x': emit_uhex(va_arg(ap, unsigned long), width, zero); break;
        case 'p':
            put_char('0'); put_char('x');
            emit_uhex(va_arg(ap, unsigned long), 16, 1);
            break;
        case '%': put_char('%'); break;
        default:  put_char('%'); put_char(*fmt); break;
        }
    }
}

/* ---- Level emit -------------------------------------------------- */

static const char *level_prefix(tm_log_level_t lvl)
{
    switch (lvl) {
    case TM_LOG_ERR:   return "[ERR ] ";
    case TM_LOG_WARN:  return "[WARN] ";
    case TM_LOG_INFO:  return "[INFO] ";
    case TM_LOG_DBG:   return "[DBG ] ";
    case TM_LOG_TRACE: return "[TRCE] ";
    }
    return "[??? ] ";
}

void tm_log_emit(tm_log_level_t lvl, const char *fmt, ...)
{
    put_str(level_prefix(lvl));
    va_list ap;
    va_start(ap, fmt);
    vemit(fmt, ap);
    va_end(ap);
    put_char('\n');
}

/* ---- Terminal failure ------------------------------------------- */

void tm_crash(const char *fmt, ...)
{
    /* Loud, unmistakable banner.  Three lines on either side so the
     * message stays visible even when interleaved with whatever else
     * was writing to the kernel-debug console at the moment of the
     * crash. */
    put_str("\n\n");
    put_str("=================================================================\n");
    put_str("*** TASKMAN CRASH ***  taskman has hit an unrecoverable error.\n");
    put_str("    Reason: ");
    va_list ap;
    va_start(ap, fmt);
    vemit(fmt, ap);
    va_end(ap);
    put_char('\n');
    put_str("    The system is halted.  The boot hart will idle in WFI.\n");
    put_str("=================================================================\n\n");

    /* Halt the boot hart.  WFI traps to S-mode on RISC-V from U-mode,
     * but the seL4 kernel just resumes us — so loop forever rather
     * than rely on the trap.  Other harts that may be running keep
     * scheduling; they hit nothing useful since taskman dispatch is
     * dead, but that's the kernel's problem now. */
    for (;;) {
        __asm__ volatile ("wfi" ::: "memory");
    }
}
