/*
 * libqsoe/src/ldisc.c — userland line discipline.
 *
 * Implements the qsoe_ldisc_* surface declared in <qsoe-system.h>.
 * Slim cooked-mode helper over a byte-stream fd — typically a UART
 * driver mounted at /dev/console or /dev/ser*.  Shared by getty,
 * login, and qsh's `read` builtin.
 *
 * No POSIX termios shim here — the LDISC vocabulary is QSOE-native
 * for v0.7.  POSIX-termios bridging is a v0.8 layer that sits ON TOP
 * of this and talks to the driver via TCGETS / TCSETS once that
 * arrives.
 *
 * Behavioural notes:
 *   - Backspace accepts both 0x08 and 0x7F regardless of verase
 *     (matches qsh + QRV behaviour — terminals send wildly mixed bytes).
 *   - Echo emits "\b \b" if echoe is set, plain "\b" if only echo set,
 *     nothing otherwise.
 *   - In canonical mode the call returns when '\n' arrives.  The
 *     newline is included in the returned bytes (POSIX semantics).
 *   - VEOF on an empty line returns 0 (real EOF).  VEOF mid-line
 *     returns the line so far without the EOF marker.
 *   - VINTR with isig set aborts readline; qsoe_errno = EINTR.
 *
 * I/O shape:
 *   The state machine pulls one byte at a time from a per-LDISC
 *   pushback buffer; the buffer is refilled by one read() of up to
 *   LDISC_BATCH_SIZE bytes from fd_in.  This amortises the IPC
 *   round-trip across a whole UART burst (e.g. a pasted line, or a
 *   multi-byte escape sequence).  Unconsumed bytes after a line
 *   terminator stay in the pushback buffer for the next call.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>

extern long read(int fd, void *buf, unsigned long n);
extern long write(int fd, const void *buf, unsigned long n);
extern void *malloc(unsigned long n);
extern void  free(void *p);

#define LDISC_BATCH_SIZE  64

struct qsoe_ldisc {
    int               fd_in;    /* keystrokes come in here       */
    int               fd_out;   /* echo / erase / line feeds out */
    qsoe_ldisc_attr_t attr;
    /* Pushback buffer.  Refilled by one read() of up to BATCH_SIZE
     * bytes; the state machine consumes from here byte-by-byte.
     * Bytes left over after a line terminator persist across calls. */
    unsigned char     pb_buf[LDISC_BATCH_SIZE];
    unsigned int      pb_pos;   /* next byte index */
    unsigned int      pb_len;   /* valid bytes in pb_buf */
};

static void ldisc_default_attr(qsoe_ldisc_attr_t *a)
{
    a->icanon = 1;
    a->echo   = 1;
    a->echoe  = 1;
    a->isig   = 1;
    a->icrnl  = 1;
    a->opost  = 1;
    a->onlcr  = 1;
    a->vintr  = 0x03;   /* ^C */
    a->verase = 0x7F;   /* DEL — 0x08 also accepted unconditionally */
    a->vkill  = 0x15;   /* ^U */
    a->veof   = 0x04;   /* ^D */
}

qsoe_ldisc_t *qsoe_ldisc_open(int fd_in, int fd_out,
                              const qsoe_ldisc_attr_t *attr)
{
    qsoe_ldisc_t *ld = (qsoe_ldisc_t *)malloc(sizeof *ld);
    if (!ld) { qsoe_errno = ENOMEM; return 0; }
    ld->fd_in  = fd_in;
    ld->fd_out = fd_out;
    ld->pb_pos = 0;
    ld->pb_len = 0;
    if (attr) ld->attr = *attr;
    else      ldisc_default_attr(&ld->attr);
    return ld;
}

void qsoe_ldisc_close(qsoe_ldisc_t *ld)
{
    if (ld) free(ld);
}

int qsoe_ldisc_get(qsoe_ldisc_t *ld, qsoe_ldisc_attr_t *out)
{
    if (!ld || !out) { qsoe_errno = EINVAL; return -1; }
    *out = ld->attr;
    return 0;
}

int qsoe_ldisc_set(qsoe_ldisc_t *ld, const qsoe_ldisc_attr_t *in)
{
    if (!ld || !in) { qsoe_errno = EINVAL; return -1; }
    ld->attr = *in;
    return 0;
}

int qsoe_ldisc_set_cooked(qsoe_ldisc_t *ld)
{
    if (!ld) { qsoe_errno = EINVAL; return -1; }
    ldisc_default_attr(&ld->attr);
    return 0;
}

int qsoe_ldisc_set_raw(qsoe_ldisc_t *ld)
{
    if (!ld) { qsoe_errno = EINVAL; return -1; }
    /* Raw mode: no line buffering, no echo, no input/output xforms,
     * no signal-on-VINTR.  Special chars stay defined but inert.  */
    ld->attr.icanon = 0;
    ld->attr.echo   = 0;
    ld->attr.echoe  = 0;
    ld->attr.isig   = 0;
    ld->attr.icrnl  = 0;
    ld->attr.opost  = 0;
    ld->attr.onlcr  = 0;
    return 0;
}

/* Internal: write `n` bytes through opost.  When opost+onlcr are set,
 * '\n' is expanded to "\r\n" before transmission.  Returns the number
 * of *input* bytes consumed (so callers can treat it like write()). */
static long ldisc_write_cooked(qsoe_ldisc_t *ld, const void *buf,
                                unsigned long n)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned long sent = 0;
    if (!ld->attr.opost || !ld->attr.onlcr) {
        long w = write(ld->fd_out, buf, n);
        return (w < 0) ? -1 : w;
    }
    for (unsigned long i = 0; i < n; ++i) {
        if (p[i] == '\n') {
            if (write(ld->fd_out, "\r\n", 2) < 0) return (long)sent ? (long)sent : -1;
        } else {
            if (write(ld->fd_out, &p[i], 1) < 0) return (long)sent ? (long)sent : -1;
        }
        ++sent;
    }
    return (long)sent;
}

long qsoe_ldisc_write(qsoe_ldisc_t *ld, const void *buf, unsigned long n)
{
    if (!ld) { qsoe_errno = EINVAL; return -1; }
    return ldisc_write_cooked(ld, buf, n);
}

/* Get one byte from the pushback buffer, refilling it with one read()
 * of up to LDISC_BATCH_SIZE bytes from fd_in if empty.  Return value:
 *    1 on success (byte stored in *c),
 *    0 on EOF on the underlying fd,
 *   -1 on read error (qsoe_errno already set by the read syscall).
 * The state machine driving canonical mode pulls bytes through this. */
static long ldisc_getbyte(qsoe_ldisc_t *ld, unsigned char *c)
{
    if (ld->pb_pos >= ld->pb_len) {
        long r = read(ld->fd_in, ld->pb_buf, LDISC_BATCH_SIZE);
        if (r <= 0) return r;
        ld->pb_pos = 0;
        ld->pb_len = (unsigned int)r;
    }
    *c = ld->pb_buf[ld->pb_pos++];
    return 1;
}

long qsoe_ldisc_readbyte(qsoe_ldisc_t *ld, unsigned char *c)
{
    if (!ld || !c) { qsoe_errno = EINVAL; return -1; }
    return ldisc_getbyte(ld, c);
}

/* Echo a single character or a special sequence.  Honours echo/echoe. */
static void ldisc_echo_byte(qsoe_ldisc_t *ld, unsigned char c)
{
    if (!ld->attr.echo) return;
    if (c == '\n' || c == '\r') {
        /* Always echo end-of-line so the cursor advances; opost handles
         * CR-NL translation if onlcr is set. */
        unsigned char nl = '\n';
        ldisc_write_cooked(ld, &nl, 1);
        return;
    }
    if (c < 0x20 || c == 0x7F) {
        /* Control char — by convention echo as "^X".  Cheap and useful
         * for VINTR / VEOF visibility. */
        unsigned char buf[2] = { '^', (unsigned char)('@' + (c & 0x1F)) };
        if (c == 0x7F) buf[1] = '?';
        ldisc_write_cooked(ld, buf, 2);
        return;
    }
    ldisc_write_cooked(ld, &c, 1);
}

static void ldisc_echo_erase(qsoe_ldisc_t *ld)
{
    /* echoe is independent of echo so a host that's already echoing
     * typed bytes (cooked TTY layer underneath qemu) can leave us in
     * charge of just the erase visuals. */
    if (ld->attr.echoe) {
        (void)write(ld->fd_out, "\b \b", 3);
    } else if (ld->attr.echo) {
        (void)write(ld->fd_out, "\b", 1);
    }
}

/* Erase the last UTF-8 codepoint from `buf` (of current length `len`),
 * returning the new length.  A codepoint is one ASCII byte (top bit 0)
 * or one lead byte followed by 1-3 continuation bytes (0b10xxxxxx).
 * Malformed sequences (lone continuation bytes) erase just one byte
 * — best-effort recovery, matches what most terminals do.
 *
 * Display width is assumed 1 column per codepoint, so one echoe
 * triplet covers the visual erase.  Wide-char (CJK / emoji) width
 * tracking is a v0.8+ refinement that needs a wcwidth-equivalent. */
static unsigned long ldisc_codepoint_backspace(const char *buf,
                                               unsigned long len)
{
    if (len == 0) return 0;
    unsigned long i = len - 1;
    /* Walk back over continuation bytes (top two bits == 10). */
    while (i > 0 && ((unsigned char)buf[i] & 0xC0) == 0x80) {
        --i;
    }
    return i;
}

long qsoe_ldisc_readline(qsoe_ldisc_t *ld, char *buf, unsigned long cap)
{
    if (!ld || !buf || cap == 0) { qsoe_errno = EINVAL; return -1; }

    /* Raw mode: drain any pushback first (left over from canonical
     * mode), then read fresh bytes straight from fd_in.  Editors
     * flipping to raw mode mid-session see no lost bytes this way. */
    if (!ld->attr.icanon) {
        if (ld->pb_pos < ld->pb_len) {
            unsigned long avail = ld->pb_len - ld->pb_pos;
            if (avail > cap) avail = cap;
            for (unsigned long i = 0; i < avail; ++i)
                buf[i] = (char)ld->pb_buf[ld->pb_pos + i];
            ld->pb_pos += (unsigned int)avail;
            return (long)avail;
        }
        return read(ld->fd_in, buf, cap);
    }

    unsigned long len = 0;
    for (;;) {
        unsigned char c;
        long r = ldisc_getbyte(ld, &c);
        if (r == 0) {
            /* EOF on the underlying fd — return whatever we have. */
            return (long)len;
        }
        if (r < 0) {
            /* Underlying read failed.  Propagate errno via -1. */
            return -1;
        }

        /* Input CR → NL translation. */
        if (c == '\r' && ld->attr.icrnl) c = '\n';

        /* Signal-bearing characters. */
        if (ld->attr.isig && c == ld->attr.vintr) {
            ldisc_echo_byte(ld, c);   /* show ^C for the user */
            qsoe_errno = EINTR;
            return -1;
        }

        /* Erase one codepoint back (UTF-8 aware). */
        if (c == ld->attr.verase || c == 0x08 || c == 0x7F) {
            if (len > 0) {
                len = ldisc_codepoint_backspace(buf, len);
                ldisc_echo_erase(ld);
            }
            continue;
        }

        /* Erase to start of line — one echo per codepoint, not per byte. */
        if (c == ld->attr.vkill) {
            while (len > 0) {
                len = ldisc_codepoint_backspace(buf, len);
                ldisc_echo_erase(ld);
            }
            continue;
        }

        /* End of file.  Mid-line: deliver line so far without the EOF
         * marker (POSIX).  Empty line: return 0 (real EOF). */
        if (c == ld->attr.veof) {
            return (long)len;
        }

        /* Regular byte.  Echo and append. */
        ldisc_echo_byte(ld, c);

        /* '\n' terminates the line; include it in the returned bytes. */
        if (c == '\n') {
            if (len < cap) buf[len++] = (char)c;
            return (long)len;
        }

        if (len < cap - 1) {
            buf[len++] = (char)c;
        }
        /* If the buffer is full before a newline arrives, swallow
         * further chars until '\n' (matches what canonical-mode
         * terminals do in practice — but bound at cap). */
    }
}
