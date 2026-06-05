/*
 * interrupt.c — QNX/QRV-compatible InterruptAttachThread family.
 *
 * Layers over libqsoe's lower-level qsoe_irq_* primitives.  v0.8
 * supports at most one IRQ attach per thread (the iid is stashed
 * in the thread's qsoe_tcb_t); multi-IRQ-per-thread would need an
 * aggregating notification, which we'll add once a real driver
 * wants it.
 *
 * Wire shape — TM_REQ_IRQ_ATTACH (0x005):
 *   request  MR0 = PLIC IRQ number, MR1 = trigger (0=level, 1=edge)
 *   reply    MR0 = handler-cap slot, MR1 = ntfn-cap slot
 *   (taskman mints both into the caller's CSpace)
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <qsoe/tls.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

/* Per-process attach table.  iid is a 1-based index (0 reserved
 * for "no attach", matching qsoe_tcb_t.irq_iid's default).  Size
 * is small — a driver process typically attaches one or two IRQs. */
#define QSOE_IRQ_ATTACH_MAX  16

typedef struct {
    int handler_slot;       /* seL4 IRQHandler cap                */
    int ntfn_slot;          /* bound Notification cap             */
    int vector;             /* for InterruptUnmask sanity check   */
    unsigned in_use;
} qsoe_irq_attach_t;

static qsoe_irq_attach_t s_attach[QSOE_IRQ_ATTACH_MAX];

static int find_free_iid(void)
{
    /* iid 0 is reserved; entries are at s_attach[iid-1]. */
    for (int i = 0; i < QSOE_IRQ_ATTACH_MAX; ++i) {
        if (!s_attach[i].in_use) return i + 1;
    }
    return 0;
}

int InterruptAttachThread(int vector, unsigned flags)
{
    (void)flags;       /* QSOE_INTR_FLAGS_* honored later; v0.8 ignores */

    int iid = find_free_iid();
    if (!iid) { qsoe_errno = ENOMEM; return -1; }

    /* Translate kernel vector → PLIC IRQ number.  Matches QRV's
     * convention (PLIC_VECTOR_BASE = 32). */
    if (vector < QSOE_PLIC_VECTOR_BASE) { qsoe_errno = EINVAL; return -1; }
    unsigned plic_irq = (unsigned)vector - QSOE_PLIC_VECTOR_BASE;
    unsigned trigger  = 0;   /* level-triggered default */

    seL4_Word mr0 = plic_irq, mr1 = trigger, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_IRQ_ATTACH, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }

    int handler_slot = (int)mr0;
    int ntfn_slot    = (int)mr1;

    /* Bind the notification to the handler so IRQs signal it. */
    if (qsoe_irq_set_notification(handler_slot, ntfn_slot) != 0) {
        return -1;       /* errno set by qsoe_irq_set_notification */
    }

    s_attach[iid - 1].handler_slot = handler_slot;
    s_attach[iid - 1].ntfn_slot    = ntfn_slot;
    s_attach[iid - 1].vector       = vector;
    s_attach[iid - 1].in_use       = 1;

    /* The calling thread becomes the IST for this IRQ.  Subsequent
     * InterruptWait() blocks on the bound notification we just set up. */
    qsoe_curthr()->irq_iid = iid;
    return iid;
}

int InterruptWait(int flags, const uint64_t *timeout)
{
    (void)timeout;     /* timed waits arrive with the MCS scheduler */
    int iid = qsoe_curthr()->irq_iid;
    if (iid <= 0 || iid > QSOE_IRQ_ATTACH_MAX ||
        !s_attach[iid - 1].in_use) {
        qsoe_errno = EINVAL;
        return -1;
    }
    int rc = qsoe_irq_wait(s_attach[iid - 1].ntfn_slot);
    if (rc != 0) return -1;

    /* QSOE_INTR_WAIT_FLAGS_UNMASK: auto-ack on the way out, saving
     * the caller an InterruptUnmask kercall.  Carries the QRV
     * intr-wait auto-unmask semantics across. */
    if (flags & QSOE_INTR_WAIT_FLAGS_UNMASK) {
        (void)qsoe_irq_ack(s_attach[iid - 1].handler_slot);
    }
    return 0;
}

int InterruptUnmask(int vector, int iid)
{
    if (iid <= 0 || iid > QSOE_IRQ_ATTACH_MAX ||
        !s_attach[iid - 1].in_use) {
        qsoe_errno = EINVAL;
        return -1;
    }
    if (s_attach[iid - 1].vector != vector) {
        qsoe_errno = EINVAL;
        return -1;
    }
    return qsoe_irq_ack(s_attach[iid - 1].handler_slot);
}

int InterruptDetach(int iid)
{
    if (iid <= 0 || iid > QSOE_IRQ_ATTACH_MAX ||
        !s_attach[iid - 1].in_use) {
        qsoe_errno = EINVAL;
        return -1;
    }
    seL4_Word mr0 = (seL4_Word)s_attach[iid - 1].handler_slot;
    seL4_Word mr1 = (seL4_Word)s_attach[iid - 1].ntfn_slot;
    seL4_Word mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_IRQ_DETACH, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }

    s_attach[iid - 1].in_use = 0;
    s_attach[iid - 1].handler_slot = 0;
    s_attach[iid - 1].ntfn_slot    = 0;
    s_attach[iid - 1].vector       = 0;
    if (qsoe_curthr()->irq_iid == iid) qsoe_curthr()->irq_iid = 0;
    return 0;
}

/* ThreadCtl lives in thread.c (handles QSOE_TCTL_NAME, _RUNMASK, _IO). */
