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
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>

extern long read(int fd, void *buf, unsigned long n);
extern long write(int fd, const void *buf, unsigned long n);
extern void *malloc(unsigned long n);
extern void  free(void *p);

#define LDISC_LINE_CAP  256

struct qsoe_ldisc {
    int               fd_in;    /* keystrokes come in here       */
    int               fd_out;   /* echo / erase / line feeds out */
    qsoe_ldisc_attr_t attr;
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

long qsoe_ldisc_readbyte(qsoe_ldisc_t *ld, unsigned char *c)
{
    if (!ld || !c) { qsoe_errno = EINVAL; return -1; }
    long r = read(ld->fd_in, c, 1);
    return r;
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

long qsoe_ldisc_readline(qsoe_ldisc_t *ld, char *buf, unsigned long cap)
{
    if (!ld || !buf || cap == 0) { qsoe_errno = EINVAL; return -1; }

    /* Raw mode: just hand bytes back unchanged, up to `cap`. */
    if (!ld->attr.icanon) {
        long r = read(ld->fd_in, buf, cap);
        return r;
    }

    unsigned long len = 0;
    unsigned long room = (cap < LDISC_LINE_CAP) ? cap : LDISC_LINE_CAP;
    for (;;) {
        unsigned char c;
        long r = read(ld->fd_in, &c, 1);
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

        /* Erase one char back. */
        if (c == ld->attr.verase || c == 0x08 || c == 0x7F) {
            if (len > 0) {
                --len;
                ldisc_echo_erase(ld);
            }
            continue;
        }

        /* Erase to start of line. */
        if (c == ld->attr.vkill) {
            while (len > 0) {
                --len;
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
            if (len < room) buf[len++] = (char)c;
            return (long)len;
        }

        if (len < room - 1) {
            buf[len++] = (char)c;
        }
        /* If the buffer is full before a newline arrives, swallow
         * further chars until '\n' (matches what canonical-mode
         * terminals do in practice — but bound at room). */
    }
}
