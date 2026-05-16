/*
 * sys/slog.h — system-logger client API.
 *
 * Compatible with QRV/QNX names: slogf, vslogf, slogb, slogi.  All
 * route through the /dev/slog resmgr served by /sbin/slogger.  The
 * library opens /dev/slog lazily on first use and stashes the fd in
 * _slogfd (extern for callers that want to bypass the wrapper).
 *
 * Event format on the wire (qsoe_slog_event_t below) is a fixed
 * 16-byte header followed by a payload (text or binary depending on
 * severity-bit-3 / flags).  Slogger writes events into a 256 KiB
 * ring buffer; sloginfo pulls them back out.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_SYS_SLOG_H
#define QSOE_SYS_SLOG_H

/* va_list — pull from stdarg.h when libc is on the include path,
 * else fall back to the compiler builtin (so resmgrs built with
 * -nostdinc can still include this header). */
#ifdef __has_include
# if __has_include(<stdarg.h>)
#  include <stdarg.h>
# else
typedef __builtin_va_list va_list;
# endif
#else
typedef __builtin_va_list va_list;
#endif

#include <qsoe-system.h>

/* Severity levels — same numeric values as QRV/QNX. */
#define _SLOG_SHUTDOWN  0   /* system about to halt */
#define _SLOG_CRITICAL  1   /* unrecoverable error */
#define _SLOG_ERROR     2   /* recoverable error */
#define _SLOG_WARNING   3   /* expected error */
#define _SLOG_NOTICE    4   /* operational note */
#define _SLOG_INFO      5   /* informational */
#define _SLOG_DEBUG1    6   /* coarse debug */
#define _SLOG_DEBUG2    7   /* fine debug */

#define _SLOG_SEVMAXVAL 0x7

/* Code packing — major (low 20 bits) | minor (top 12 bits).
 * Used in <sys/slogcodes.h> for the canonical major-code list. */
#define _SLOG_SETCODE(major, minor)   ((unsigned)(major) | ((unsigned)(minor) << 20))
#define _SLOG_GETMAJOR(code)          ((code) & 0xfffffu)
#define _SLOG_GETMINOR(code)          (((code) >> 20) & 0xfffu)

/* On-disk / wire event format (16-byte fixed header + variable
 * payload).  Bytes 14-15 reserved for future flag bits. */
#define QSOE_SLOG_MAGIC      0x534cu   /* 'SL' */
#define QSOE_SLOG_FLAG_TEXT  0x1u      /* payload is human-readable text */

typedef struct {
    uint16_t magic;        /* QSOE_SLOG_MAGIC */
    uint8_t  severity;
    uint8_t  flags;
    uint32_t code;
    uint64_t time_us;      /* wall-clock microseconds since boot */
    uint16_t pid;          /* source pid */
    uint16_t paylen;       /* payload bytes following this header */
} qsoe_slog_event_t;

/* Maximum payload bytes per event. */
#define QSOE_SLOG_MAX_PAYLOAD   240

extern int _slogfd;        /* lazy-opened /dev/slog fd */

int slogf (int code, int severity, const char *fmt, ...);
int vslogf(int code, int severity, const char *fmt, va_list ap);
int slogb (int code, int severity, void *data, int size);
int slogi (int code, int severity, int nargs, ...);

#endif /* QSOE_SYS_SLOG_H */
