/*
 * sync_r.c — _r ("reentrant", errno-free) Sync* wrappers for LQ.
 *
 * The shared libc body's locks.c and putc.h call SyncMutexLock_r /
 * SyncMutexUnlock_r on hot paths (every fputc, every FILE lock).  The
 * _r suffix is the QNX convention for "return raw rc, do NOT touch
 * errno" -- a contract these hot paths rely on so libc internals
 * never have their errno clobbered by a successful putc.
 *
 * LQ's libqsoe historically provided only the errno-setting forms
 * (SyncMutexLock returns int, errno set on failure).  These thin
 * wrappers save/restore qsoe_errno around the existing implementation
 * so the contract holds without duplicating the lock-loop body.  The
 * return-value translation (int rc/errno -> long raw rc) is moot
 * because both call sites in the shared body ignore the result.
 *
 * Phase 2.5 (libqsoe fold into lq/libc/) absorbs the lock body
 * directly here and this file's save/restore disappears in favour of
 * a single shared core called by both _r and non-_r forms.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/qsoe.h>

long SyncMutexLock_r(sync_t *s)
{
    int saved = qsoe_errno;
    int r = SyncMutexLock(s);
    if (r == 0) return 0;
    long neg_err = -(long) qsoe_errno;
    qsoe_errno = saved;
    return neg_err;
}

long SyncMutexUnlock_r(sync_t *s)
{
    int saved = qsoe_errno;
    int r = SyncMutexUnlock(s);
    if (r == 0) return 0;
    long neg_err = -(long) qsoe_errno;
    qsoe_errno = saved;
    return neg_err;
}
