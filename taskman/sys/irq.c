/*
 * sys/irq.c — TM_REQ_IRQ_ATTACH / TM_REQ_IRQ_DETACH handlers.
 *
 * Runtime IRQ-attach for the QNX/QRV-compatible Interrupt* API.
 * Caller asks for a PLIC IRQ; taskman mints (IRQHandler, Notification)
 * caps directly into the caller's CSpace and returns the slot
 * numbers.  The library wires them via qsoe_irq_set_notification.
 *
 * v0.7's IRQ wiring was magic-named (devc-ser8250 got UART caps
 * pre-minted at spawn time).  v0.8 makes IRQ attach generic so any
 * driver can claim any PLIC line at runtime — needed by pci-server
 * and future devb-*.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "irq.h"
#include "../sel4_syscalls.h"
#include "../sel4_types.h"
#include "../qsoe_invoke.h"
#include "../proc/proc.h"

/* Mint a fresh (IRQHandler, Notification) cap pair into the caller's
 * CSpace.  Returns 0 on success with the slot numbers in *out_handler
 * and *out_ntfn; -errno on failure. */
int tm_irq_attach(pid_t caller, unsigned plic_irq, unsigned trigger,
                  seL4_CPtr *out_handler, seL4_CPtr *out_ntfn)
{
    tm_process_t *proc = tm_process_lookup(caller);
    if (!proc) return -ESRCH;

    seL4_CPtr   handler_slot = tm_process_alloc_slot(caller);
    seL4_CPtr   ntfn_slot    = tm_process_alloc_slot(caller);
    seL4_Uint8  depth        = cnode_depth_for(caller);

    /* (1) IRQHandler cap from the kernel's IRQControl.  Minted
     * directly into the caller's CSpace at handler_slot. */
    seL4_Word err = qsoe_irq_control_get(seL4_CapIRQControl,
                                          (seL4_Word)plic_irq,
                                          (seL4_Word)trigger,
                                          proc->cnode, handler_slot,
                                          depth);
    if (err) {
        return -EINVAL;
    }

    /* (2) Notification — retype taskman's RAM untyped into a fresh
     * Notification object placed in the caller's CSpace at
     * ntfn_slot. */
    err = qsoe_untyped_retype(s_untyped, seL4_NotificationObject,
                               seL4_NotificationBits,
                               proc->cnode, 0, 0,
                               ntfn_slot, 1);
    if (err) {
        /* Best-effort cleanup of the handler cap. */
        (void)qsoe_cnode_delete(proc->cnode, handler_slot, depth);
        return -ENOMEM;
    }

    *out_handler = handler_slot;
    *out_ntfn    = ntfn_slot;
    return 0;
}

/* Tear down the cap pair installed by tm_irq_attach.  Doesn't
 * reclaim the Notification's underlying memory — that stays in
 * taskman's untyped budget until the process exits — but does
 * revoke the IRQ binding so the line is free for other claimants. */
int tm_irq_detach(pid_t caller, seL4_CPtr handler_slot, seL4_CPtr ntfn_slot)
{
    tm_process_t *proc = tm_process_lookup(caller);
    if (!proc) return -ESRCH;
    seL4_Uint8 depth = cnode_depth_for(caller);

    /* Revoking the handler clears its IRQ binding in the kernel. */
    (void)qsoe_cnode_revoke(proc->cnode, handler_slot, depth);
    (void)qsoe_cnode_delete(proc->cnode, handler_slot, depth);
    (void)qsoe_cnode_delete(proc->cnode, ntfn_slot,    depth);
    return 0;
}
