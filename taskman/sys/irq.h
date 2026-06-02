/* sys/irq.h — runtime IRQ-attach handlers.  See sys/irq.c. */
#ifndef QSOE_TASKMAN_IRQ_H
#define QSOE_TASKMAN_IRQ_H

#include "../sel4_types.h"
#include <qsoe-system.h>

int tm_irq_attach(pid_t caller, unsigned plic_irq, unsigned trigger,
                  seL4_CPtr *out_handler, seL4_CPtr *out_ntfn);

int tm_irq_detach(pid_t caller, seL4_CPtr handler_slot, seL4_CPtr ntfn_slot);

#endif
