/*
 * spawn.h — taskman's process spawner.
 *
 * v0.3.0: load a single ELF blob into a fresh VSpace + CSpace, configure
 * a new TCB, resume. See plan.md §3.
 */
#ifndef QSOE_TASKMAN_SPAWN_H
#define QSOE_TASKMAN_SPAWN_H

#include "sel4_types.h"
#include "../libqsoe/include/qsoe/qrv.h"

/*
 * tm_spawn — spawn a new user-space process from an in-memory ELF.
 *
 *   elf, elf_len : the ELF blob in taskman's address space
 *   pid          : the pid taskman is assigning to the new process
 *                  (also becomes the badge on the new process's
 *                   QSOE_CAP_TASKMAN_EP cap)
 *   primary_ep   : taskman's primary endpoint cap (we mint a badged
 *                  Send cap from this into the new process's CSpace
 *                  slot QSOE_CAP_TASKMAN_EP)
 *
 * Returns 0 on success, negative errno on failure.
 */
int tm_spawn(const void *elf, unsigned long elf_len,
             pid_t pid, seL4_CPtr primary_ep);

#endif /* QSOE_TASKMAN_SPAWN_H */
