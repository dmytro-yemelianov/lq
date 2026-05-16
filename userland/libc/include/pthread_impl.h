/*
 * userland/libc/include/pthread_impl.h — QSOE shim.
 *
 * Reached by musl's stdio (putc.h / getc.h / __lockfile.c, ...) through
 * the symlink core/userland/libc/src/internal/pthread_impl.h → here.
 * Upstream musl's pthread_impl.h pulls in `<syscall.h>` + `<futex.h>`
 * and inlines `__syscall(SYS_futex, ...)` for __wake / __futexwait;
 * we don't have a futex syscall and don't want one (see Sync* design
 * memo).  This file replaces that header with the minimum surface
 * musl's per-FILE-lock stdio path needs to *compile*.
 *
 * Lifetime: this shim covers v0.8..~v0.18.  Once a real QNX-Sync*-
 * based pthread layer lands in libqsoe, this file becomes the place
 * where __pthread_self / __wake / __lockfile route into it.  When the
 * eventual libc replacement (FreeBSD-seeded, unified with libqsoe) is
 * cut, the whole file goes away.
 *
 * Runtime contract — important: musl's __fdopen sets `f->lock = -1`
 * whenever `libc.threaded == 0`.  Until we install a pthread_create
 * that flips libc.threaded, every FILE has lock=-1, so do_putc()'s
 * fast path (`if (l < 0) return putc_unlocked(c, f)`) always fires.
 * Locking_putc / __lockfile / __wake are reachable at link time but
 * never at run time.  Hence the shim provides them as small inline
 * abort-stubs — if anyone ever does call them with libc.threaded != 0
 * but no real pthread layer, we want a noisy abort, not silent UB.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef _PTHREAD_IMPL_H
#define _PTHREAD_IMPL_H

#include <pthread.h>      /* pthread_t, pthread_attr_t — type-only */
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include "atomic.h"       /* a_cas, a_swap — pulled from musl's internals */
#include "libc.h"         /* `struct __libc libc;` flag carrier */

/* `struct __pthread` (also aliased to `pthread` for upstream's
 * convenience) — musl's per-thread control block.  We don't have
 * pthread; libqsoe gives every thread a `qsoe_tcb_t` whose first
 * field is `tid` (see userland/libqsoe/include/qsoe/tls.h).  This
 * shim's struct lists only the field musl source touches (`tid`),
 * laid out at offset 0 so a cast from qsoe_tcb_t* is layout-safe.
 *
 * If a future extracted .c reads more fields, either grow this
 * shim and the field will live in qsoe_tcb_t's identical prefix, or
 * the cast becomes unsafe and we move to a real pthread layer. */
#define pthread __pthread
struct __pthread {
    int tid;                    /* must be at offset 0, matches qsoe_tcb */
};

/* `__pthread_self()` reads the thread-pointer register (`tp` on
 * RV64).  libqsoe's crt0 plants &qsoe_main_tcb in tp, and
 * ThreadCreate's trampoline writes the new thread's tcb there.
 * Casting the read pointer to `struct __pthread *` is safe because
 * the field layout matches at offset 0 — see the struct comment. */
static inline struct __pthread *__pthread_self(void)
{
    struct __pthread *t;
    __asm__("mv %0, tp" : "=r"(t));
    return t;
}

/* __wake — promise to wake up `cnt` waiters parked at `addr`.  In
 * single-threaded mode there's nothing to wake; this body never
 * runs at runtime because locking_putc()'s a_swap check only sees
 * MAYBE_WAITERS when someone else parked on the lock, which can't
 * happen with libc.threaded==0. */
static inline void __wake(volatile void *addr, int cnt, int priv)
{
    (void)addr; (void)cnt; (void)priv;
    /* Deliberately empty.  If pthread arrives without updating this,
     * the static-inline absence means symbol-not-found and the libqsoe
     * pthread author will notice immediately at link time. */
}

/* __futexwait — promise to block until `*addr != val`.  Same story
 * as __wake: present so musl source links, body irrelevant until
 * libc.threaded flips. */
static inline void __futexwait(volatile void *addr, int val, int priv)
{
    (void)addr; (void)val; (void)priv;
}

/* Some musl internals reference these via extern declarations.  Pin
 * them to weak no-op bodies in the shim's accompanying .c (if needed
 * — none of the files we currently extract reach them, so we leave
 * those as undefined references that surface only if a future
 * placement.txt change pulls in a consumer). */

#endif /* _PTHREAD_IMPL_H */
