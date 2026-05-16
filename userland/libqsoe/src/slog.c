/*
 * slog.c — client-side slogf / vslogf / slogb / slogi.
 *
 * Lazy-opens /dev/slog on first use, then writes events as a single
 * write() per call (16-byte header + payload, total <= 256 bytes).
 * If /dev/slog isn't available (slogger not yet up), the call still
 * succeeds but the event is dropped — early-boot resmgrs can call
 * slogf() before slogger is registered without faulting.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>
#include <sys/slog.h>

/* libqsoe builds with -nostdinc, so use the compiler-builtin
 * va_list machinery rather than <stdarg.h>. */
#ifndef va_start
# define va_start(ap, last) __builtin_va_start(ap, last)
# define va_arg(ap, type)   __builtin_va_arg(ap, type)
# define va_end(ap)         __builtin_va_end(ap)
#endif

extern int   open  (const char *path, int flags, ...);
extern long  write (int fd, const void *buf, unsigned long count);

int _slogfd = -1;

/* Minimal vsnprintf — handles %s %d %u %x %p %c %% and width %08x.
 * Returns the number of bytes written (not counting the NUL). */
static int qs_vsnprintf(char *buf, unsigned long cap, const char *fmt,
                        va_list ap)
{
    unsigned long n = 0;
    if (cap == 0) return 0;
    cap -= 1;   /* room for NUL */

    for (; *fmt && n < cap; ++fmt) {
        if (*fmt != '%') { buf[n++] = *fmt; continue; }
        ++fmt;
        if (*fmt == 0) break;
        /* Optional flags: '0' for zero-pad. */
        int zero = 0, width = 0;
        if (*fmt == '0') { zero = 1; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            ++fmt;
        }
        /* Optional length modifier 'l' (ignored — all ints are word-sized). */
        while (*fmt == 'l') ++fmt;

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && n < cap) buf[n++] = *s++;
            break;
        }
        case 'c': {
            int c = va_arg(ap, int);
            if (n < cap) buf[n++] = (char)c;
            break;
        }
        case 'd': {
            long v = va_arg(ap, long);
            int neg = 0;
            unsigned long u;
            if (v < 0) { neg = 1; u = (unsigned long)(-v); } else u = (unsigned long)v;
            char d[24]; int di = 0;
            if (u == 0) d[di++] = '0';
            else while (u) { d[di++] = (char)('0' + (u % 10)); u /= 10; }
            if (neg && n < cap) buf[n++] = '-';
            while (di > 0 && n < cap) buf[n++] = d[--di];
            break;
        }
        case 'u': {
            unsigned long u = va_arg(ap, unsigned long);
            char d[24]; int di = 0;
            if (u == 0) d[di++] = '0';
            else while (u) { d[di++] = (char)('0' + (u % 10)); u /= 10; }
            while (di > 0 && n < cap) buf[n++] = d[--di];
            break;
        }
        case 'x':
        case 'p': {
            unsigned long u = va_arg(ap, unsigned long);
            char d[20]; int di = 0;
            if (u == 0) d[di++] = '0';
            else while (u) {
                unsigned q = (unsigned)(u & 0xf);
                d[di++] = (char)(q < 10 ? '0' + q : 'a' + q - 10);
                u >>= 4;
            }
            if (*fmt == 'p' && n + 1 < cap) { buf[n++] = '0'; if (n < cap) buf[n++] = 'x'; }
            if (zero && width > di) {
                for (int p = 0; p < width - di && n < cap; ++p) buf[n++] = '0';
            }
            while (di > 0 && n < cap) buf[n++] = d[--di];
            break;
        }
        case '%':
            if (n < cap) buf[n++] = '%';
            break;
        default:
            if (n < cap) buf[n++] = '%';
            if (n < cap) buf[n++] = *fmt;
            break;
        }
    }
    buf[n] = 0;
    return (int)n;
}

/* Lazy /dev/slog open.  Returns the fd or -1 on failure. */
static int ensure_slog_fd(void)
{
    if (_slogfd >= 0) return _slogfd;
    /* O_WRONLY = 1 in QSOE; if not available, the open returns -1
     * and we silently drop events.  No need to retry per call —
     * one open attempt per process is enough; slogger comes up
     * before init.sh exec's qsh, so callers from qsh onward
     * succeed. */
    _slogfd = open("/dev/slog", 1, 0);
    return _slogfd;
}

/* Wall-clock microseconds via ClockTime (CLOCK_REALTIME = 0). */
static uint64_t now_us(void)
{
    unsigned long t = 0;
    ClockTime(0, 0, &t);
    return (uint64_t)t / 1000u;
}

int vslogf(int code, int severity, const char *fmt, va_list ap)
{
    int fd = ensure_slog_fd();
    if (fd < 0) return 0;   /* drop silently */

    /* Assemble header + text payload in a single buffer.       */
    unsigned char buf[sizeof(qsoe_slog_event_t) + QSOE_SLOG_MAX_PAYLOAD];
    qsoe_slog_event_t *h = (qsoe_slog_event_t *)buf;
    h->magic    = QSOE_SLOG_MAGIC;
    h->severity = (uint8_t)(severity & 0x7);
    h->flags    = QSOE_SLOG_FLAG_TEXT;
    h->code     = (uint32_t)code;
    h->time_us  = now_us();
    h->pid      = (uint16_t)qsoe_self_pid;
    h->paylen   = 0;

    char *text = (char *)(h + 1);
    int n = qs_vsnprintf(text, QSOE_SLOG_MAX_PAYLOAD, fmt, ap);
    h->paylen = (uint16_t)n;

    unsigned long total = sizeof(qsoe_slog_event_t) + (unsigned long)n;
    long w = write(fd, buf, total);
    return (w == (long)total) ? 0 : -1;
}

int slogf(int code, int severity, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vslogf(code, severity, fmt, ap);
    va_end(ap);
    return r;
}

int slogb(int code, int severity, void *data, int size)
{
    if (size < 0 || size > QSOE_SLOG_MAX_PAYLOAD) {
        qsoe_errno = EINVAL;
        return -1;
    }
    int fd = ensure_slog_fd();
    if (fd < 0) return 0;

    unsigned char buf[sizeof(qsoe_slog_event_t) + QSOE_SLOG_MAX_PAYLOAD];
    qsoe_slog_event_t *h = (qsoe_slog_event_t *)buf;
    h->magic    = QSOE_SLOG_MAGIC;
    h->severity = (uint8_t)(severity & 0x7);
    h->flags    = 0;       /* binary */
    h->code     = (uint32_t)code;
    h->time_us  = now_us();
    h->pid      = (uint16_t)qsoe_self_pid;
    h->paylen   = (uint16_t)size;

    if (size > 0 && data) {
        unsigned char *dst = (unsigned char *)(h + 1);
        const unsigned char *src = (const unsigned char *)data;
        for (int i = 0; i < size; ++i) dst[i] = src[i];
    }
    unsigned long total = sizeof(qsoe_slog_event_t) + (unsigned long)size;
    long w = write(fd, buf, total);
    return (w == (long)total) ? 0 : -1;
}

int slogi(int code, int severity, int nargs, ...)
{
    if (nargs < 0 || nargs > 8) { qsoe_errno = EINVAL; return -1; }
    int data[8];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; ++i) data[i] = va_arg(ap, int);
    va_end(ap);
    return slogb(code, severity, data, (int)(nargs * sizeof(int)));
}
