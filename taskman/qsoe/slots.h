/*
 * <qsoe/slots.h> — QSOE/L (seL4) well-known CSpace slot + cap-lifecycle
 * conventions, shared by the LQ libc seam and taskman.
 *
 * seL4-specific: NQ/Skimmer has no capabilities and never includes this
 * header.  It lives in the LQ tree (lq/taskman/qsoe/, on both the libc
 * seam's and taskman's include path) so the OS-independent libc/ tree
 * stays free of seL4 concepts.  See Design doc §4.5 "CSpace conventions".
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_SLOTS_H
#define QSOE_SLOTS_H

#include <qsoe/tm_msgs.h>   /* TM_REQ_VARIANT_BASE (variant-private opcodes) */

#define QSOE_CAP_NULL          0
#define QSOE_CAP_TASKMAN_EP    1   /* Send cap to taskman's primary endpoint */
#define QSOE_CAP_OWN_UNTYPED   2   /* This process's untyped budget */

/* Spawn-time stdio inheritance. taskman mints three badged Send-caps to
 * (taskman, CONSOLE_CHID) into these slots; libqsoe's init binds them to
 * fds 0/1/2 so musl's stdin/stdout/stderr work before main() runs. The
 * slots are stable across the program's lifetime — close() on fds 0/1/2
 * deletes the cap but leaves the slot for re-allocation by the libqsoe
 * slot bookkeeping. */
#define QSOE_CAP_STDIN_CONNECT  3
#define QSOE_CAP_STDOUT_CONNECT 4
#define QSOE_CAP_STDERR_CONNECT 5

/* Driver-special-case slots. taskman mints these into devc-* children at
 * spawn (gated on the ELF name for now; manifest-driven cap granting
 * comes later). Stay within QSOE_CAP_WELL_KNOWN_END. */
#define QSOE_CAP_IRQ_HANDLER    6  /* IRQHandler for the driver's IRQ line */
#define QSOE_CAP_UART_FRAME     7  /* 4 KiB device-untyped covering UART MMIO */
#define QSOE_CAP_IRQ_NTFN       8  /* Notification the IRQHandler is bound to */

/* Cap to the process's own CNode root.  Lets a user-space resmgr defer
 * replies from inside its own process (mirrors taskman's
 * tm_process_waitpid park pattern) until data arrives.  devc-ser8250
 * uses this to park blocking readers; the IRQ thread later wakes them.
 * Depth on the child side is 12 — the radix of the freshly-retyped
 * CNode. */
#define QSOE_CAP_CNODE_SELF     9
#define QSOE_CAP_CNODE_DEPTH    12

/* Slot 10 is reserved within the well-known range — QSOE/L claims it for
 * the seL4-MCS reply object (see <sel4_types.h>: QSOE_CAP_REPLY). */

#define QSOE_CAP_WELL_KNOWN_END 16 /* slots [2..15] reserved; dynamics start at 16 */

/* LQ-private taskman opcode, in the variant opcode space (see
 * TM_REQ_VARIANT_BASE in <qsoe/tm_msgs.h>).  Second half of POSIX
 * close(2): after libc has sent TM_REQ_CLOSE on the fd's bound cap (the
 * resmgr notifies on it), libc sends this to ask taskman to delete the
 * cap from the caller's CSpace and free the connection-table entry.
 * Lives here, not in the shared opcode table, because deleting a CSpace
 * cap is a seL4 concept.
 *   MR0 = caller-CSpace slot of the cap being dropped. */
#define TM_REQ_DETACH_CAP       (TM_REQ_VARIANT_BASE + 1u)  /* LQ variant op 1 */

/* LQ-private taskman opcode, in the variant opcode space (see
 * TM_REQ_VARIANT_BASE in <qsoe/tm_msgs.h>).  Per-process signal-thread
 * plumbing: rebind a channel's pulse Notification from the process's
 * main TCB (where tm_channel_create binds it by default) to the
 * non-main system thread that will park in MsgReceive on it.  On seL4 a
 * signaled Notification is delivered to its BOUND TCB, so without this
 * the signal thread would never wake on a kill() pulse.  Lives here,
 * not in the shared table, because binding a Notification to a TCB is a
 * seL4 concept (NQ/Skimmer has no equivalent; its hook is a no-op).
 *   MR0 = owner chid of the signal channel.
 *   MR1 = tid of the system thread to bind the Notification to. */
#define TM_REQ_CHANNEL_BIND_THREAD (TM_REQ_VARIANT_BASE + 2u)  /* LQ variant op 2 */

#endif /* QSOE_SLOTS_H */
