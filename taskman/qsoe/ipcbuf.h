/*
 * qsoe/ipcbuf.h: the seL4 message-register IPC buffer, LQ-private.
 *
 * qsoe_ipcbuf_t mirrors seL4_IPCBuffer: tag (the MessageInfo), the
 * message-register words (msg[0..3] ride registers a2-a5 on the fast
 * path, the rest spill here), the extra-caps slots, and the receive-path
 * CNode descriptor.  It is the native shape of seL4 IPC and therefore
 * lives ONLY on LQ -- Skimmer (NQ) does pure byte-copy IPC with no
 * message registers and never needs this struct.  The shared
 * <sys/qsoe.h> keeps the kernel-neutral wire constants
 * (QSOE_MSG_MAX_LENGTH, QSOE_MSG_MAX_EXTRA_CAPS) and the generic
 * `void *ipcbuf` field in qsoe_tcb_t; only the seL4-shaped overlay and
 * the dereference macro are LQ-local.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_LQ_IPCBUF_H
#define QSOE_LQ_IPCBUF_H

#include <sys/qsoe.h>   /* QSOE_MSG_MAX_LENGTH, QSOE_MSG_MAX_EXTRA_CAPS,
                         * qsoe_curthr(), qsoe_tcb_t.ipcbuf */

typedef struct {
    unsigned long tag;
    unsigned long msg[QSOE_MSG_MAX_LENGTH];
    unsigned long userData;
    unsigned long caps_or_badges[QSOE_MSG_MAX_EXTRA_CAPS];
    unsigned long receiveCNode;
    unsigned long receiveIndex;
    unsigned long receiveDepth;
} qsoe_ipcbuf_t;

/* The per-thread seL4 IPC buffer, reached through the TCB's generic
 * pointer.  qsoe_libc_init() / the thread seam write the buffer VA into
 * qsoe_curthr()->ipcbuf before any IPC runs. */
#define qsoe_ipcbuf    ((qsoe_ipcbuf_t *)qsoe_curthr()->ipcbuf)

#endif /* QSOE_LQ_IPCBUF_H */
