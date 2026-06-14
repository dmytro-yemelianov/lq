/*
 * tm_kdbg.h — LQ taskman's kernel-debug-console seam.
 *
 * The leveled logging API (tm_err/warn/info/dbg/trace, tm_log,
 * tm_log_init, tm_log_apply_cmdline) is OS-independent and lives in
 * the shared <tm_log.h> (libtaskman/src/log.c).  This header carries
 * only the two LQ-specific primitives that sit alongside it on seL4,
 * plus the one-time hook that points the shared logger at the seL4
 * debug console.
 *
 * Direct calls to sel4_debug_putchar / sel4_debug_puts are FORBIDDEN
 * anywhere in taskman except inside tm_log.c (the singular
 * implementation site).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_TM_KDBG_H
#define QSOE_TASKMAN_TM_KDBG_H

/* Point the shared logger's sink at the seL4 debug console.  Call
 * once, FIRST thing in main(), before any tm_* logging — until then
 * the shared tm_log() has nowhere to write and drops silently. */
void tm_log_console_init(void);

/* Raw byte emit for the /dev/console resmgr's write path.  Bypasses
 * level filtering and any formatting; just shoves the byte at the
 * kernel debug console.  Must NOT be used for logging — use the
 * tm_* level macros from <tm_log.h> instead. */
void tm_raw_putc(char c);

/* Terminal-failure exit for taskman.  Emits a banner the operator
 * cannot miss (`*** TASKMAN CRASH ***`), prints the printf-style
 * reason, then halts the boot hart forever (wfi in a tight loop).
 * Self-contained — it does not depend on the shared logger being
 * healthy, so it still speaks during early-boot failures.  Never
 * returns. */
__attribute__((noreturn))
void tm_crash(const char *fmt, ...);

#endif /* QSOE_TASKMAN_TM_KDBG_H */
