/*
 * libqsoe/src/irq.c — userland-driver interrupt surface.
 *
 * Thin wrappers around the three kernel invocations a driver's IRQ
 * thread needs (set-notification / wait / ack).  Drivers — and only
 * drivers — get to call these; everything else in userland sees
 * channels and connections, never IRQs.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

int qsoe_irq_set_notification(int handler, int ntfn)
{
    seL4_Word err = qsoe_irq_handler_set_notification((seL4_CPtr)handler,
                                                       (seL4_CPtr)ntfn);
    if (err) { qsoe_errno = (int)err; return -1; }
    return 0;
}

int qsoe_irq_wait(int ntfn)
{
    (void)qsoe_sys_wait((seL4_CPtr)ntfn);
    return 0;
}

int qsoe_irq_ack(int handler)
{
    seL4_Word err = qsoe_irq_handler_ack((seL4_CPtr)handler);
    if (err) { qsoe_errno = (int)err; return -1; }
    return 0;
}
