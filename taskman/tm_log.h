/*
 * tm_log.h — taskman's structured-logging API.
 *
 * Every taskman message goes through one of the five level macros
 * below.  Today they all funnel into sel4_debug_putchar() (the
 * kernel-debug SBI putchar — the only way to talk to a console
 * before the UART driver is up).  Once the in-process trace buffer
 * lands, the same call will *also* append a binary record there,
 * so production runs can grep WARN/ERR after the fact instead of
 * needing a serial capture at the moment of the event.
 *
 * Levels follow Linux's pr_* shape:
 *
 *   tm_err  — unrecoverable problem; usually followed by a halt.
 *   tm_warn — recoverable problem worth flagging.
 *   tm_info — normal milestone ("syscfg built", "spawning init", …).
 *   tm_dbg  — verbose detail for development; off in production once
 *             severity-filtering lands.
 *   tm_trace— hottest paths (per-spawn / per-IPC).  Off by default.
 *
 * All five take a printf-lite format string.  Supported conversions:
 *   %s %c %d %u %x %p %%
 *   length modifier `l` is accepted (and ignored — long is the
 *   register width on RV64 anyway)
 *   width + `0` zero-pad for %x  ("%016lx", "%08x", …)
 *
 * A trailing newline is appended automatically by tm_log_emit().
 *
 * Direct calls to sel4_debug_putchar / sel4_debug_puts are FORBIDDEN
 * anywhere in taskman except inside tm_log.c (the singular
 * implementation site).  The /dev/console early-boot data path uses
 * tm_raw_putc() instead, which lives in the same module so it can
 * eventually fan out through the same trace machinery.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_TM_LOG_H
#define QSOE_TASKMAN_TM_LOG_H

typedef enum {
    TM_LOG_ERR   = 0,
    TM_LOG_WARN  = 1,
    TM_LOG_INFO  = 2,
    TM_LOG_DBG   = 3,
    TM_LOG_TRACE = 4,
} tm_log_level_t;

/* Single emit entry point.  Don't call directly — use the macros. */
void tm_log_emit(tm_log_level_t lvl, const char *fmt, ...);

/* Raw byte emit for the /dev/console resmgr's write path.  Bypasses
 * level filtering and any prefix; just shoves the byte at the kernel
 * debug console.  Must NOT be used for logging — see the level macros. */
void tm_raw_putc(char c);

/* Terminal-failure exit for taskman.  Emits a banner the operator
 * cannot miss (`*** TASKMAN CRASH ***`), prints the printf-style
 * reason, then halts the boot hart forever (wfi in a tight loop).
 * Used in place of the historical `for (;;) __asm__ volatile("nop");`
 * spin so future log readers / the kernel-debug capture both
 * surface that taskman gave up rather than livelocked.  Never
 * returns. */
__attribute__((noreturn))
void tm_crash(const char *fmt, ...);

/* Level macros.  Each expands to a single tm_log_emit call; usable
 * anywhere a regular function call would fit, including inside
 * expressions/statements with single semicolons. */
#define tm_err(...)   tm_log_emit(TM_LOG_ERR,   __VA_ARGS__)
#define tm_warn(...)  tm_log_emit(TM_LOG_WARN,  __VA_ARGS__)
#define tm_info(...)  tm_log_emit(TM_LOG_INFO,  __VA_ARGS__)
#define tm_dbg(...)   tm_log_emit(TM_LOG_DBG,   __VA_ARGS__)
#define tm_trace(...) tm_log_emit(TM_LOG_TRACE, __VA_ARGS__)

#endif /* QSOE_TASKMAN_TM_LOG_H */
