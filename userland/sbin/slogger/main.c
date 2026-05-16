/*
 * /sbin/slogger — QSOE system logger.
 *
 * Single userspace process that owns a ring buffer of slog events.
 * Registers /dev/slog with the path manager.  Any process writes via
 * the standard write(_slogfd, &event, ...) path (libqsoe's slogf()
 * does this); sloginfo / other consumers read events back via read().
 *
 * Storage: one 64 KiB byte-ring; events stored back-to-back as
 * (qsoe_slog_event_t header) + payload.  Drop-oldest policy when
 * full (older events evicted to make room for the new one).
 *
 * v0.8-rc1 scope:
 *   * IO_WRITE  — append event to the ring
 *   * IO_READ   — drain N events into caller's buffer
 *   * FSTAT     — char-device shape so isatty(open("/dev/slog")) is 0
 *   * OPEN/CLOSE — implicit (taskman mints connection caps)
 *
 * No file-backed mirror yet (the QRV logger thread that streams
 * /dev/slog to a file lives in v0.9 once fs-qrv lands).
 *
 * Per CLAUDE.md "Requirements for resource managers" — only libqsoe
 * + libc; no direct seL4 syscalls.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sys/slog.h>

#include "../../taskman/sel4_types.h"

#define SLOG_PATH        "/dev/slog"
#define SLOG_RING_BYTES  (64 * 1024)

static unsigned char g_ring[SLOG_RING_BYTES];
static unsigned      g_head;       /* next-byte-to-read offset */
static unsigned      g_tail;       /* next-byte-to-write offset */
static unsigned      g_used;       /* bytes currently in ring */

static unsigned char s_reply_buf[32 + 928];

/* ---------- ring helpers ---------------------------------------- */

static void ring_evict(unsigned bytes)
{
    /* Drop whole events from the head until at least `bytes` are
     * free.  Events are sized via their 16-byte header's `paylen`
     * field; advance head by header+paylen each time. */
    while (g_used + bytes > SLOG_RING_BYTES) {
        if (g_used < sizeof(qsoe_slog_event_t)) break;
        qsoe_slog_event_t h;
        unsigned char *hp = (unsigned char *)&h;
        for (unsigned i = 0; i < sizeof h; ++i) {
            hp[i] = g_ring[(g_head + i) % SLOG_RING_BYTES];
        }
        unsigned evt_size = (unsigned)sizeof h + h.paylen;
        if (evt_size > g_used) evt_size = g_used;   /* corruption guard */
        g_head  = (g_head + evt_size) % SLOG_RING_BYTES;
        g_used -= evt_size;
    }
}

static void ring_append(const unsigned char *src, unsigned len)
{
    if (len > SLOG_RING_BYTES) return;
    if (g_used + len > SLOG_RING_BYTES) ring_evict(len);
    for (unsigned i = 0; i < len; ++i) {
        g_ring[(g_tail + i) % SLOG_RING_BYTES] = src[i];
    }
    g_tail  = (g_tail + len) % SLOG_RING_BYTES;
    g_used += len;
}

/* Pop up to `cap` bytes of contiguous events from the head.  Returns
 * the byte count actually written into `dst`.  Stops at event
 * boundaries so the caller gets whole events. */
static unsigned ring_drain(unsigned char *dst, unsigned cap)
{
    unsigned written = 0;
    while (g_used >= sizeof(qsoe_slog_event_t)) {
        qsoe_slog_event_t h;
        unsigned char *hp = (unsigned char *)&h;
        for (unsigned i = 0; i < sizeof h; ++i) {
            hp[i] = g_ring[(g_head + i) % SLOG_RING_BYTES];
        }
        unsigned evt_size = (unsigned)sizeof h + h.paylen;
        if (evt_size > g_used) break;
        if (written + evt_size > cap) break;
        for (unsigned i = 0; i < evt_size; ++i) {
            dst[written + i] = g_ring[(g_head + i) % SLOG_RING_BYTES];
        }
        g_head  = (g_head + evt_size) % SLOG_RING_BYTES;
        g_used -= evt_size;
        written += evt_size;
    }
    return written;
}

/* ---------- IPC handlers --------------------------------------- */

static int slog_send_read_reply(int rcvid, unsigned want)
{
    if (want > 928) want = 928;
    unsigned got = ring_drain(&s_reply_buf[32], want);
    for (unsigned i = 0; i < 32; ++i) s_reply_buf[i] = 0;
    s_reply_buf[0] = (unsigned char)( got       & 0xff);
    s_reply_buf[1] = (unsigned char)((got >> 8) & 0xff);
    s_reply_buf[2] = (unsigned char)((got >>16) & 0xff);
    s_reply_buf[3] = (unsigned char)((got >>24) & 0xff);
    return MsgReply(rcvid, 0, s_reply_buf, 32 + got);
}

static int slog_send_stat_reply(int rcvid)
{
    tm_stat_t *st = (tm_stat_t *)&s_reply_buf[32];
    unsigned char *zero = (unsigned char *)st;
    for (unsigned i = 0; i < sizeof *st; ++i) zero[i] = 0;
    st->st_dev     = 7;
    st->st_ino     = 1;
    st->st_mode    = TM_S_IFCHR | 0666;
    st->st_nlink   = 1;
    /* Linux misc-class char device, like /dev/log used to be on
     * older systems; major=10 is MISC_MAJOR.  Minor 100 is unused
     * in mainline Linux and reserved here for QSOE slogger. */
    st->st_rdev    = (10UL << 8) | 100;
    st->st_blksize = 256;
    unsigned want  = (unsigned)sizeof *st;
    for (unsigned i = 0; i < 32; ++i) s_reply_buf[i] = 0;
    s_reply_buf[0] = (unsigned char)( want       & 0xff);
    s_reply_buf[1] = (unsigned char)((want >> 8) & 0xff);
    s_reply_buf[2] = (unsigned char)((want >>16) & 0xff);
    s_reply_buf[3] = (unsigned char)((want >>24) & 0xff);
    return MsgReply(rcvid, 0, s_reply_buf, 32 + sizeof *st);
}

/* ---------- main --------------------------------------------- */

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("[slogger] alive, pid=%d\n", (int)qsoe_self_pid);
    fflush(stdout);

    int chid = ChannelCreate(0);
    if (chid < 0) {
        printf("[slogger] ChannelCreate failed\n");
        return 1;
    }
    if (qsoe_pathmgr_register(SLOG_PATH, chid) != 0) {
        printf("[slogger] pathmgr_register(%s) failed: errno=%d\n",
               SLOG_PATH, qsoe_errno);
        return 1;
    }
    printf("[slogger] %s registered (chid=%d, ring=%d bytes)\n",
           SLOG_PATH, chid, SLOG_RING_BYTES);
    fflush(stdout);

    if (procmgr_detach(0) != 0) {
        printf("[slogger] procmgr_detach failed\n");
    }

    /* Main dispatch loop. */
    for (;;) {
        struct _msg_info info;
        unsigned char dummy[8] = { 0 };
        int rcvid = MsgReceive(chid, dummy, sizeof dummy, &info);
        if (rcvid == -1) continue;
        if (info.flags & QSOE_MI_PULSE) continue;

        unsigned mr0 = (unsigned)qsoe_ipcbuf->msg[0];

        switch (info.label) {
        case TM_REQ_IO_WRITE: {
            unsigned nbytes = mr0;
            const unsigned char *src =
                (const unsigned char *)&qsoe_ipcbuf->msg[4];
            ring_append(src, nbytes);
            /* Reply: bytes consumed = bytes written (we never push back). */
            for (unsigned i = 0; i < 32; ++i) s_reply_buf[i] = 0;
            s_reply_buf[0] = (unsigned char)( nbytes       & 0xff);
            s_reply_buf[1] = (unsigned char)((nbytes >> 8) & 0xff);
            s_reply_buf[2] = (unsigned char)((nbytes >>16) & 0xff);
            s_reply_buf[3] = (unsigned char)((nbytes >>24) & 0xff);
            MsgReply(rcvid, 0, s_reply_buf, 32);
            break;
        }
        case TM_REQ_IO_READ: {
            unsigned want = mr0 ? mr0 : 928;
            slog_send_read_reply(rcvid, want);
            break;
        }
        case TM_REQ_FSTAT:
            slog_send_stat_reply(rcvid);
            break;
        case TM_REQ_CLOSE:
            MsgReply(rcvid, 0, 0, 0);
            break;
        default:
            MsgReply(rcvid, ENOSYS, 0, 0);
            break;
        }
    }
}
