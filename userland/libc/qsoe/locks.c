/*
 * locks.c — stdio + open-file-list locking primitives.
 *
 *   __lock / __unlock     — generic int spinlock used by ofl.c and a
 *                            few other musl internals.
 *   __lockfile / __unlockfile
 *                          — per-FILE flockfile-style locking, used by
 *                            FLOCK/FUNLOCK in stdio.
 *
 * Upstream musl implements all four with atomic ops + __futexwait.
 * The futex path is exactly what we excised, so we provide no-op
 * stubs here.  This is correct for QSOE today (every program is
 * single-threaded until Stream B); when libqsoe exposes real
 * pthread/futex equivalents the bodies become real mutex calls.
 *
 * Signatures use `void *` for the FILE-pointer cases so this file
 * needs no musl-internal headers — the linker resolves by name
 * irrespective of argument types in C.
 */

void __lock(volatile int *l);
void __lock(volatile int *l)
{
    (void)l;
}

void __unlock(volatile int *l);
void __unlock(volatile int *l)
{
    (void)l;
}

int __lockfile(void *f);
int __lockfile(void *f)
{
    (void)f;
    return 0;  /* "did not take the lock" → callers skip __unlockfile */
}

void __unlockfile(void *f);
void __unlockfile(void *f)
{
    (void)f;
}
