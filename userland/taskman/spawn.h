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
 *   argc, argv   : argument vector strings (v0.4.4). argv pointers
 *                  must remain valid for the duration of the spawn
 *                  call; they're copied onto the child's initial
 *                  stack per the RISC-V SysV ABI. argc==0 is allowed
 *                  (boot-time spawn of tester passes 0).
 *   envc, envp   : environment vector strings; same lifetime/encoding
 *                  rules as argv.
 *
 * Returns 0 on success, negative errno on failure.
 */
int tm_spawn(const void *elf, unsigned long elf_len,
             pid_t pid, seL4_CPtr primary_ep,
             int argc, const char *const *argv,
             int envc, const char *const *envp,
             const char *elf_name);

/* v0.6.1: register the device-untyped slot covering the 16550 UART
 * MMIO so spawn.c can hand it to devc-ser8250 at process creation.
 * main.c calls this once at boot after find_device_untyped_for_paddr
 * resolves the cap slot. 0 = no UART device-untyped available. */
void tm_set_uart_untyped(seL4_CPtr ut_slot);

/* v0.6.4 Memory Manager: serve one TM_REQ_MMAP.
 *
 * Rounds `len` up to a multiple of 2 MiB, allocates that many
 * Mega_Pages from taskman's main untyped pool, maps them contiguously
 * into the caller's VSpace starting at the process's mmap_top cursor,
 * advances mmap_top, returns the base vaddr.
 *
 * Returns 0 on success and writes the base vaddr to *out_vaddr;
 * negative errno on failure. */
int tm_mmap_serve(pid_t caller, unsigned long len, unsigned long *out_vaddr);

#endif /* QSOE_TASKMAN_SPAWN_H */
