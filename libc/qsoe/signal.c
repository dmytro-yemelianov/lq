/*
 * lq/libc/qsoe/signal.c -- LQ seam for the shared system-thread bring-up.
 *
 * The OS-independent signals-as-pulses machinery lives in the shared
 * libc body: __qsoe_syschan_init() (libc/qsoe/sys_thread.c) creates the
 * signal channel, registers it with taskman, and spawns the per-process
 * system thread that parks in MsgReceive and runs handlers via
 * __qsoe_sig_deliver (libc/qsoe/sigaction.c).
 *
 * Only one step is kernel-specific.  On seL4, tm_channel_create binds a
 * channel's pulse Notification to the process's MAIN TCB, and a signaled
 * Notification is delivered to its bound TCB -- so a kill() pulse would
 * wake main, not the system thread parked in MsgReceive.  This seam
 * implements __qsoe_syschan_bind() by asking taskman to move that
 * binding onto the system thread's TCB (TM_REQ_CHANNEL_BIND_THREAD, an
 * LQ variant-private opcode; see <qsoe/slots.h>).  Skimmer needs no such
 * move, so its __qsoe_syschan_bind (nq/libc/libc_init.c) is a no-op.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/sigdeliver.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

void __qsoe_syschan_bind(int chid, int tid)
{
    seL4_Word mr0 = (seL4_Word)chid, mr1 = (seL4_Word)tid, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag =
        seL4_MessageInfo_new(TM_REQ_CHANNEL_BIND_THREAD, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                             &mr0, &mr1, &mr2, &mr3);
    if (seL4_MessageInfo_get_label(reply) != 0) {
        /* Non-fatal: the process simply won't receive signals on the
         * system thread.  Announce so it isn't a silent dead end. */
        static const char m[] =
            "libc: syschan: signal-thread notification bind failed\n";
        (void) write(STDERR_FILENO, m, sizeof m - 1);
    }
}
