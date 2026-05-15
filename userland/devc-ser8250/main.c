/*
 * devc-ser8250 — 16550 UART resource manager.
 *
 * v0.6.1: QSOE's first userland-process resmgr. Spawned by /sbin/init,
 * driven by PLIC interrupts via a dedicated higher-priority thread.
 *
 * Architecture (matches QRV's pattern, in clean-room re-implementation):
 *
 *   main thread          IRQ thread (separate TCB, hart 1)
 *   -----------          -----------
 *   uart_init()
 *   ThreadCreate(irq)    seL4_Wait(IRQ_NTFN)        ← blocks
 *   ChannelCreate                                      ↑ kernel
 *   pathmgr_register                                   ↑ signals
 *   procmgr_detach(0)                                  ↑ on PLIC IRQ
 *   loop:
 *     seL4_Recv          uart_drain_rx -> ring
 *     IO_WRITE: tx       irq_handler_ack
 *     IO_READ: pop ring  loop
 *
 * TX is polled (the FIFO absorbs writes cheaply). RX is interrupt
 * driven; the IRQ thread drains the hardware FIFO into a software
 * ring and the main thread pops from the ring on IO_READ. If the
 * ring is empty, IO_READ returns 0 bytes (EAGAIN-ish) — full blocking
 * reads with line discipline are deferred to v0.6.x.
 */

#include <stdio.h>
#include "../taskman/sel4_syscalls.h"
#include "../taskman/sel4_types.h"
#include "../taskman/qsoe_invoke.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"
#include "../libqsoe/include/qsoe/wire.h"
#include "uart.h"
#include "ring.h"

/* Shared RX ring between the IRQ thread (producer) and the main
 * thread (consumer). The TX path is polled, no ring needed. */
static struct ser_ring g_rx_ring;

/* v0.6.4 RX-park state (mirrors QRV's pending_rcvid pattern).
 *
 * g_pulse_coid       : side-channel self-connection coid the IRQ
 *                      thread MsgSendPulses on to wake the main
 *                      thread when it has filled the ring.
 * g_pending_*        : at most one blocking reader at a time.  Slot
 *                      holds the SaveCaller'd reply cap; want is
 *                      the byte count the caller asked for.  Both
 *                      0 means no reader is parked.
 *
 * Pulse code carried in MsgSendPulse — we only ever send one
 * meaningful code so the value is informational; main thread checks
 * pending state, not the code. */
#define PULSE_CODE_RX_READY  1
static int           g_pulse_coid = -1;
static unsigned long g_pending_reader_slot;
static unsigned      g_pending_reader_want;

/* Forward decls. */
void *uart_irq_thread(void *arg);
extern unsigned long qsoe_state_alloc_empty_slot(void);
extern void          qsoe_state_free_empty_slot(unsigned long slot);

/* Drain whatever pulse records taskman queued on our channel when
 * the IRQ thread sent the wake pulse — otherwise the queue fills
 * after a few sends and future MsgSendPulse calls return EAGAIN.
 * We don't care about the contents, just that the queue is empty. */
static void devc_drain_pulse_queue(seL4_CPtr recv_slot)
{
    for (;;) {
        seL4_Word p_mr0 = (seL4_Word)recv_slot, p_mr1 = 0, p_mr2 = 0, p_mr3 = 0;
        seL4_MessageInfo_t p_tag = seL4_MessageInfo_new(TM_REQ_PULSE_FETCH,
                                                        0, 0, 1);
        seL4_MessageInfo_t p_reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, p_tag,
                                                   &p_mr0, &p_mr1, &p_mr2, &p_mr3);
        if (seL4_MessageInfo_get_label(p_reply) != 0) break;
    }
}

/* Deliver bytes from the RX ring to the parked reader (Send on the
 * SaveCaller'd reply slot) and clear the park state.  Caller must
 * have verified that pending_reader_slot != 0 AND the ring has at
 * least one byte. */
static void devc_deliver_to_parked(void)
{
    unsigned want = g_pending_reader_want;
    if (want > 928) want = 928;
    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    unsigned got = ser_ring_drain(&g_rx_ring, dst, want);
    if (got == 0) return;  /* should not happen — caller checked */

    seL4_Word mr0 = (seL4_Word)got, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_Word reply_len = 4 + (got + 7) / 8;
    seL4_MessageInfo_t reply = seL4_MessageInfo_new(0, 0, 0, reply_len);
    qsoe_sys_send(g_pending_reader_slot, reply, mr0, mr1, mr2, mr3);

    qsoe_state_free_empty_slot(g_pending_reader_slot);
    g_pending_reader_slot = 0;
    g_pending_reader_want = 0;
}

/* Bind the IRQ Notification to our own (main thread's) TCB.
 * Without this the kernel has nowhere to deliver the signal. After
 * we bind, we ThreadCreate the IRQ thread and rebind the
 * Notification to it — but ThreadBind doesn't have a "rebind"
 * primitive; we'd have to TCB_UnbindNotification first. For v0.6.1
 * simplicity we bind to the IRQ thread's TCB once it exists. */

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* Driver self-test announce before hardware touches anything. */
    printf("[devc-ser8250] alive, pid=%d\n", (int)qsoe_self_pid);
    fflush(stdout);

    /* Initialise the ring before the IRQ thread runs. */
    ser_ring_init(&g_rx_ring);

    /* Bring up the UART hardware. */
    uart_init();
    printf("[devc-ser8250] 16550 initialised @ vaddr 0xA00000\n");
    fflush(stdout);

    /* Wire the IRQ handler to its Notification (both pre-minted by
     * spawn.c at QSOE_CAP_IRQ_HANDLER and QSOE_CAP_IRQ_NTFN). */
    seL4_Word rc = qsoe_irq_handler_set_notification(QSOE_CAP_IRQ_HANDLER,
                                                      QSOE_CAP_IRQ_NTFN);
    if (rc != 0) {
        printf("[devc-ser8250] IRQ SetNotification failed: %lu\n",
               (unsigned long)rc);
        fflush(stdout);
        return 1;
    }

    /* Spawn the IRQ thread. Pinned to hart 1 (away from hart 0 where
     * the main dispatch sits). Higher priority isn't easily setable
     * via ThreadCreate today; the same priority is fine for v0.6.1. */
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

    /* Open our serving channel and announce ourselves at /dev/ser1. */
    int chid = ChannelCreate(0);
    if (chid < 0) {
        printf("[devc-ser8250] ChannelCreate failed\n");
        fflush(stdout);
        return 1;
    }
    if (qsoe_pathmgr_register("/dev/ser1", chid) != 0) {
        printf("[devc-ser8250] pathmgr_register failed: errno=%d\n", qsoe_errno);
        fflush(stdout);
        return 1;
    }
    printf("[devc-ser8250] /dev/ser1 registered (chid=%d)\n", chid);
    fflush(stdout);

    /* v0.6.4 self-connect for the IRQ-thread → main-thread wake.
     * Side-channel flag keeps this connection out of the fd
     * namespace (lives in [bit 30] coid range, same as SYSMGR_COID).
     * The pulse arrives at our own channel as a bound-Notification
     * signal — Recv wakes with badge & QSOE_NTFN_BADGE_BIT. */
    g_pulse_coid = ConnectAttach(ND_LOCAL_NODE, qsoe_self_pid, chid,
                                  0, QSOE_SIDE_CHANNEL);
    if (g_pulse_coid < 0) {
        printf("[devc-ser8250] ConnectAttach(self) failed: errno=%d\n",
               qsoe_errno);
        fflush(stdout);
        return 1;
    }

    /* "Stay resident": tell taskman we're ready. init's waitpid()
     * unblocks at this point. */
    if (procmgr_detach(0) != 0) {
        printf("[devc-ser8250] procmgr_detach failed\n");
        fflush(stdout);
    }

    /* Main dispatch loop.  Three wake sources, all funnelled through
     * one seL4_Recv on our serving EP:
     *
     *   1. Client IPC (TM_REQ_IO_WRITE / IO_READ): badge = client scoid,
     *      QSOE_NTFN_BADGE_BIT *not* set.  Normal request/reply.
     *   2. IRQ-thread wake pulse (RX bytes arrived): badge has
     *      QSOE_NTFN_BADGE_BIT set (the bound Notification fired).
     *      We drain the pulse queue and, if a reader is parked,
     *      Send the deferred reply on the saved slot.
     *   3. Unknown badge: ENOSYS.
     *
     * When a TM_REQ_IO_READ finds the ring empty, SaveCaller saves
     * the implicit reply cap; we set need_reply=0 and re-Recv
     * without replying — the deferred Send will eventually unblock
     * the client.  Single-reader: a second blocking read while
     * another is parked returns EBUSY. */
    extern unsigned long qsoe_state_chid_to_slot(int chid);
    seL4_CPtr recv_slot = (seL4_CPtr)qsoe_state_chid_to_slot(chid);

    seL4_Word badge = 0;
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t info = qsoe_sys_recv(recv_slot, &badge,
                                            &mr0, &mr1, &mr2, &mr3);
    for (;;) {
        seL4_Word err = 0;
        seL4_Word reply_len = 0;
        seL4_Word r0 = 0, r1 = 0, r2 = 0, r3 = 0;
        int need_reply = 1;

        if (badge & QSOE_NTFN_BADGE_BIT) {
            /* Wake from bound Notification — the IRQ thread pulsed
             * us because the RX ring has data. */
            devc_drain_pulse_queue(recv_slot);
            if (g_pending_reader_slot != 0 && !ser_ring_empty(&g_rx_ring)) {
                devc_deliver_to_parked();
            }
            need_reply = 0;  /* nothing to reply to — Notification, not IPC */
        } else {
            unsigned label = (unsigned)seL4_MessageInfo_get_label(info);
            switch (label) {
            case TM_REQ_IO_WRITE: {
                unsigned nbytes = (unsigned)mr0;
                unsigned char *src = (unsigned char *)&qsoe_ipcbuf->msg[4];
                for (unsigned i = 0; i < nbytes; ++i) {
                    uart_tx_byte(src[i]);
                }
                r0 = (seL4_Word)nbytes;
                reply_len = 1;
                break;
            }
            case TM_REQ_IO_READ: {
                unsigned want = (unsigned)mr0;
                if (want == 0) want = 1;
                if (want > 928) want = 928;
                unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
                unsigned got = ser_ring_drain(&g_rx_ring, dst, want);
                if (got > 0) {
                    r0 = (seL4_Word)got;
                    reply_len = 4 + (got + 7) / 8;
                    break;
                }
                /* Ring empty — park (QRV-style). */
                if (g_pending_reader_slot != 0) {
                    err = EBUSY;
                    break;
                }
                unsigned long slot = qsoe_state_alloc_empty_slot();
                if (slot == 0) { err = ENOMEM; break; }
                if (qsoe_cnode_save_caller(QSOE_CAP_CNODE_SELF, slot,
                                           QSOE_CAP_CNODE_DEPTH) != 0) {
                    qsoe_state_free_empty_slot(slot);
                    err = EAGAIN;
                    break;
                }
                g_pending_reader_slot = slot;
                g_pending_reader_want = want;
                /* Race re-check: the IRQ thread may have filled the
                 * ring and pulsed *between* our empty check and the
                 * SaveCaller.  If so, deliver inline now and leave
                 * the dispatch loop in the no-reply state. */
                if (!ser_ring_empty(&g_rx_ring)) {
                    devc_deliver_to_parked();
                }
                need_reply = 0;
                break;
            }
            default:
                err = ENOSYS;
                break;
            }
        }

        if (need_reply) {
            seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, reply_len);
            mr0 = r0; mr1 = r1; mr2 = r2; mr3 = r3;
            info = qsoe_sys_reply_recv(recv_slot, reply, &badge,
                                       &mr0, &mr1, &mr2, &mr3);
        } else {
            info = qsoe_sys_recv(recv_slot, &badge,
                                 &mr0, &mr1, &mr2, &mr3);
        }
    }
}

void *uart_irq_thread(void *arg)
{
    (void)arg;
    for (;;) {
        /* Block on the IRQ notification. The kernel signals it on
         * each rising edge of PLIC line 10. */
        (void)qsoe_sys_wait(QSOE_CAP_IRQ_NTFN);

        /* Drain whatever the UART FIFO has into the ring. */
        unsigned drained = uart_drain_rx(&g_rx_ring);

        /* v0.6.4 wake the main thread (mirrors QRV's IST →
         * MsgSendPulse(pulse_coid, PULSE_CODE_RX) path).  Only fire
         * the pulse when we actually moved data — empty wakes
         * (spurious IRQs, double-fire after a missed unmask) would
         * just churn the bound Notification. */
        if (drained > 0 && g_pulse_coid >= 0) {
            (void)MsgSendPulse(g_pulse_coid, 10, PULSE_CODE_RX_READY, 0);
        }

        /* Tell the kernel we serviced this interrupt; re-arm. */
        (void)qsoe_irq_handler_ack(QSOE_CAP_IRQ_HANDLER);
    }
}
