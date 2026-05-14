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

/* Forward decl. */
void *uart_irq_thread(void *arg);

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

    /* "Stay resident": tell taskman we're ready. init's waitpid()
     * unblocks at this point. */
    if (procmgr_detach(0) != 0) {
        printf("[devc-ser8250] procmgr_detach failed\n");
        fflush(stdout);
    }

    /* Main dispatch loop. ReplyRecv pattern on our own channel. */
    extern unsigned long qsoe_state_chid_to_slot(int chid);
    seL4_CPtr recv_slot = (seL4_CPtr)qsoe_state_chid_to_slot(chid);

    seL4_Word badge;
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t info = qsoe_sys_recv(recv_slot, &badge,
                                             &mr0, &mr1, &mr2, &mr3);
    for (;;) {
        unsigned label = (unsigned)seL4_MessageInfo_get_label(info);
        seL4_Word err = 0;
        seL4_Word reply_len = 0;
        seL4_Word r0 = 0, r1 = 0, r2 = 0, r3 = 0;

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
            if (want > 928) want = 928;
            unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
            unsigned got = ser_ring_drain(&g_rx_ring, dst, want);
            r0 = (seL4_Word)got;
            reply_len = 4 + (got + 7) / 8;
            break;
        }
        default:
            /* Unknown label — bounce back with an error. */
            err = ENOSYS;
            break;
        }

        seL4_MessageInfo_t reply = seL4_MessageInfo_new(err, 0, 0, reply_len);
        mr0 = r0; mr1 = r1; mr2 = r2; mr3 = r3;
        info = qsoe_sys_reply_recv(recv_slot, reply, &badge,
                                    &mr0, &mr1, &mr2, &mr3);
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
        (void)uart_drain_rx(&g_rx_ring);

        /* Tell the kernel we serviced this interrupt; re-arm. */
        (void)qsoe_irq_handler_ack(QSOE_CAP_IRQ_HANDLER);
    }
}
