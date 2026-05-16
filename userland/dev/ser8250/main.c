/*
 * devc-ser8250 — 16550 UART resource manager.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 *
 * Serial port resource manager.
 * Driven by PLIC interrupts via a dedicated higher-priority thread.
 *
 * Architecture (matches QRV's pattern, in clean-room re-implementation):
 *
 *   main thread          IRQ thread (separate TCB, hart 1)
 *   -----------          -----------
 *   uart_init()
 *   ThreadCreate(irq)    qsoe_irq_wait(NTFN)           ← blocks
 *   ChannelCreate                                        ↑ kernel
 *   pathmgr_register                                     ↑ signals
 *   procmgr_detach(0)                                    ↑ on PLIC IRQ
 *   loop:
 *     MsgReceive         uart_drain_rx → ring
 *     IO_WRITE: tx       MsgSendPulse(main)
 *     IO_READ:  pop ring qsoe_irq_ack
 *
 * TX is polled (the FIFO absorbs writes cheaply).  RX is interrupt
 * driven; the IRQ thread drains the hardware FIFO into a software
 * ring and the main thread pops from the ring on IO_READ.  If the
 * ring is empty, the read is parked via MsgSavereply and woken when
 * the next IRQ-thread pulse arrives.
 */

#include <stdio.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "uart.h"
#include "ring.h"

/* Shared RX ring between the IRQ thread (producer) and the main
 * thread (consumer).  The TX path is polled, no ring needed. */
static struct ser_ring g_rx_ring;

/* RX-park state (mirrors QRV's pending_rcvid pattern).
 *
 * g_pulse_coid     : side-channel self-connection coid the IRQ thread
 *                    MsgSendPulses on to wake the main thread when it
 *                    has filled the ring.
 * g_pending_rcvid  : at most one blocking reader at a time.  The value
 *                    is a saved rcvid (QSOE_RCVID_SAVED bit set) handed
 *                    back by MsgSavereply.  0 means no parked reader.
 * g_pending_want   : byte count the parked reader asked for. */
#define PULSE_CODE_RX_READY  1
static int      g_pulse_coid = -1;
static int      g_pending_rcvid;
static unsigned g_pending_want;

void *uart_irq_thread(void *arg);

/* Build a wire-shape IO_READ reply (mr0 = byte count, payload at
 * msg[4..]) and send it via MsgReply.  Works for both the immediate
 * reply path (rcvid is a normal badge) and the parked-reader path
 * (rcvid has QSOE_RCVID_SAVED set) — MsgReply distinguishes. */
static unsigned char s_reply_buf[32 + 928];
static int devc_send_read_reply(int rcvid, unsigned want)
{
    if (want > 928) want = 928;
    unsigned got = ser_ring_drain(&g_rx_ring, &s_reply_buf[32], want);
    /* Header: mr0 = byte count, mr1..mr3 = 0. */
    for (unsigned i = 0; i < 32; ++i) s_reply_buf[i] = 0;
    s_reply_buf[0] = (unsigned char)(got       & 0xff);
    s_reply_buf[1] = (unsigned char)((got>> 8) & 0xff);
    s_reply_buf[2] = (unsigned char)((got>>16) & 0xff);
    s_reply_buf[3] = (unsigned char)((got>>24) & 0xff);
    return MsgReply(rcvid, 0, s_reply_buf, 32 + got);
}

/* Deliver bytes from the RX ring to the parked reader (if any) and
 * clear the park state.  Caller must have verified that the ring is
 * non-empty AND g_pending_rcvid != 0. */
static void devc_deliver_to_parked(void)
{
    int rcvid = g_pending_rcvid;
    unsigned want = g_pending_want;
    g_pending_rcvid = 0;
    g_pending_want  = 0;
    (void)devc_send_read_reply(rcvid, want);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("[devc-ser8250] alive, pid=%d\n", (int)qsoe_self_pid);
    fflush(stdout);

    ser_ring_init(&g_rx_ring);

    uart_init();
    printf("[devc-ser8250] 16550 initialised @ vaddr 0xA00000\n");
    fflush(stdout);

    /* Wire the IRQ handler to its Notification (both pre-minted by
     * spawn.c at QSOE_CAP_IRQ_HANDLER and QSOE_CAP_IRQ_NTFN). */
    if (qsoe_irq_set_notification(QSOE_CAP_IRQ_HANDLER,
                                   QSOE_CAP_IRQ_NTFN) != 0) {
        printf("[devc-ser8250] qsoe_irq_set_notification failed: errno=%d\n",
               qsoe_errno);
        fflush(stdout);
        return 1;
    }

    /* Spawn the IRQ thread, pinned to hart 1 to keep it away from
     * the main dispatch on hart 0. */
    struct _thread_attr at = { 0 };
    at.runmask = 0x2;
    int irq_tid = ThreadCreate(0, uart_irq_thread, 0, &at);
    if (irq_tid < 0) {
        printf("[devc-ser8250] ThreadCreate(irq) failed\n");
        fflush(stdout);
        return 1;
    }
    printf("[devc-ser8250] IRQ thread spawned, tid=%d\n", irq_tid);
    fflush(stdout);

    int chid = ChannelCreate(0);
    if (chid < 0) {
        printf("[devc-ser8250] ChannelCreate failed\n");
        fflush(stdout);
        return 1;
    }
    if (qsoe_pathmgr_register("/dev/ser1", chid) != 0) {
        printf("[devc-ser8250] pathmgr_register failed: errno=%d\n",
               qsoe_errno);
        fflush(stdout);
        return 1;
    }
    printf("[devc-ser8250] /dev/ser1 registered (chid=%d)\n", chid);
    fflush(stdout);

    /* Self-connect for the IRQ-thread → main-thread wake.  The
     * side-channel flag keeps this coid out of the fd namespace; the
     * pulse arrives back at our own channel as a bound-Notification
     * signal that MsgReceive surfaces with QSOE_MI_PULSE. */
    g_pulse_coid = ConnectAttach(ND_LOCAL_NODE, qsoe_self_pid, chid,
                                  0, QSOE_SIDE_CHANNEL);
    if (g_pulse_coid < 0) {
        printf("[devc-ser8250] ConnectAttach(self) failed: errno=%d\n",
               qsoe_errno);
        fflush(stdout);
        return 1;
    }

    /* Tell taskman we're ready.  init's waitpid() returns at this
     * point with our exit status; we keep running as a daemon. */
    if (procmgr_detach(0) != 0) {
        printf("[devc-ser8250] procmgr_detach failed\n");
        fflush(stdout);
    }

    /* Main dispatch loop.  Two wake sources, both surfaced by
     * MsgReceive:
     *
     *   1. Client IPC (TM_REQ_IO_WRITE / IO_READ) — info.flags == 0,
     *      info.label carries the wire-protocol tag.  Reply via
     *      MsgReply(rcvid, ...).
     *   2. IRQ-thread pulse — info.flags & QSOE_MI_PULSE.  Nothing
     *      to reply to; if a reader is parked AND the ring has data,
     *      deliver via the saved rcvid.
     *
     * If a TM_REQ_IO_READ finds the ring empty, MsgSavereply()
     * preserves the implicit reply cap into a fresh slot and returns
     * a stable rcvid; we store it in g_pending_rcvid.  Single
     * reader: a second blocking read while one is already parked
     * returns EBUSY. */
    for (;;) {
        struct _msg_info info;
        unsigned char dummy[8] = { 0 };   /* mr0..mr3 spill, ignored */
        int rcvid = MsgReceive(chid, dummy, sizeof dummy, &info);
        if (rcvid < 0 && (unsigned)rcvid != (unsigned)-1) {
            /* Unusual: MsgReceive returned a negative non-(-1) value.
             * Shouldn't happen; skip. */
            continue;
        }
        if (rcvid == -1) continue;  /* transient libqsoe error */

        if (info.flags & QSOE_MI_PULSE) {
            /* Pulse — try to drain into any parked reader. */
            if (g_pending_rcvid != 0 && !ser_ring_empty(&g_rx_ring)) {
                devc_deliver_to_parked();
            }
            continue;
        }

        unsigned mr0 = (unsigned)qsoe_ipcbuf->msg[0];

        switch (info.label) {
        case TM_REQ_IO_WRITE: {
            unsigned nbytes = mr0;
            const unsigned char *src =
                (const unsigned char *)&qsoe_ipcbuf->msg[4];
            for (unsigned i = 0; i < nbytes; ++i) uart_tx_byte(src[i]);
            /* Reply: mr0 = bytes consumed.  Use the 32-byte header
             * convention via s_reply_buf so we don't need a special
             * "MR-only" reply primitive. */
            for (unsigned i = 0; i < 32; ++i) s_reply_buf[i] = 0;
            s_reply_buf[0] = (unsigned char)( nbytes       & 0xff);
            s_reply_buf[1] = (unsigned char)((nbytes >> 8) & 0xff);
            s_reply_buf[2] = (unsigned char)((nbytes >>16) & 0xff);
            s_reply_buf[3] = (unsigned char)((nbytes >>24) & 0xff);
            MsgReply(rcvid, 0, s_reply_buf, 32);
            break;
        }

        case TM_REQ_FSTAT: {
            /* Report ourselves as a character device so isatty()
             * returns 1 in the client.  Without this, qsh's main()
             * skips the SF_TTY path and the in-house line editor
             * (with arrow-key history) never engages on /dev/ser1. */
            tm_stat_t *st = (tm_stat_t *)&s_reply_buf[32];
            unsigned char *zero = (unsigned char *)st;
            for (unsigned i = 0; i < sizeof *st; ++i) zero[i] = 0;
            st->st_dev     = 5;             /* synthetic — matches console */
            st->st_ino     = 2;
            st->st_mode    = TM_S_IFCHR | 0666;
            st->st_nlink   = 1;
            st->st_rdev    = (5UL << 8) | 2;
            st->st_blksize = 256;
            unsigned want = (unsigned)sizeof *st;
            for (unsigned i = 0; i < 32; ++i) s_reply_buf[i] = 0;
            s_reply_buf[0] = (unsigned char)( want       & 0xff);
            s_reply_buf[1] = (unsigned char)((want >> 8) & 0xff);
            s_reply_buf[2] = (unsigned char)((want >>16) & 0xff);
            s_reply_buf[3] = (unsigned char)((want >>24) & 0xff);
            MsgReply(rcvid, 0, s_reply_buf, 32 + sizeof *st);
            break;
        }

        case TM_REQ_IO_READ: {
            unsigned want = mr0;
            if (want == 0) want = 1;
            if (!ser_ring_empty(&g_rx_ring)) {
                devc_send_read_reply(rcvid, want);
                break;
            }
            /* Ring empty — park (one slot only). */
            if (g_pending_rcvid != 0) {
                MsgReply(rcvid, EBUSY, 0, 0);
                break;
            }
            int saved = MsgSavereply(rcvid);
            /* MsgSavereply returns -1 on error; a successful return
             * has the QSOE_RCVID_SAVED bit (0x80000000) set, which
             * makes the value negative as a signed int.  Check the
             * sentinel exactly, not just "negative". */
            if (saved == -1) {
                MsgReply(rcvid, EAGAIN, 0, 0);
                break;
            }
            g_pending_rcvid = saved;
            g_pending_want  = want;
            /* Race re-check: the IRQ thread may have filled the ring
             * between the empty check and the SaveCaller.  If so,
             * deliver inline now and clear the park. */
            if (!ser_ring_empty(&g_rx_ring)) {
                devc_deliver_to_parked();
            }
            break;
        }

        default:
            MsgReply(rcvid, ENOSYS, 0, 0);
            break;
        }
    }
}

void *uart_irq_thread(void *arg)
{
    (void)arg;
    for (;;) {
        /* Block until the kernel signals our IRQ Notification on a
         * rising edge of PLIC line 10. */
        qsoe_irq_wait(QSOE_CAP_IRQ_NTFN);

        unsigned drained = uart_drain_rx(&g_rx_ring);

        /* Wake the main thread only when we actually moved data.
         * Empty wakes (spurious / double-fire after a missed unmask)
         * would just churn the bound Notification. */
        if (drained > 0 && g_pulse_coid >= 0) {
            (void)MsgSendPulse(g_pulse_coid, 10, PULSE_CODE_RX_READY, 0);
        }

        (void)qsoe_irq_ack(QSOE_CAP_IRQ_HANDLER);
    }
}
