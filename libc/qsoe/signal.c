/*
 * lq/libc/qsoe/signal.c -- LQ signal subsystem bootstrap.
 *
 * Provides qsoe_signal_init(), the per-kernel entry that
 * _qsoe_start_main() calls before main() to wire up signal delivery.
 * The POSIX surface (signal, sigaction, kill, raise, sigprocmask, ...)
 * comes from the shared libc body (libc/qsoe/sigaction.c and the
 * musl-derived 1/signal.c family).
 *
 * v0 stub: signal-thread bring-up needs a per-process pulse-bearing
 * channel bound to a NON-main TCB, which depends on a
 * CHANNEL_CREATE_BOUND_TO_CALLER taskman opcode that doesn't exist
 * yet.  Until then this is a no-op -- the signal API compiles and
 * links cleanly, but signals are not actually delivered.  Tracked
 * by the LQ v0.7 signal-delivery ticket.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

int qsoe_signal_init(void);
int qsoe_signal_init(void)
{
    return 0;
}
