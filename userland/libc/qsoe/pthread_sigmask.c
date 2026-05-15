/*
 * pthread_sigmask.c — POSIX pthread_sigmask() and sigprocmask().
 *
 * Maintains a per-thread sigset_t in qsoe_curthr()->sig_mask.
 * v0.7's signal delivery (libqsoe's signal thread) doesn't yet
 * consult the mask before pulsing a thread, but the storage and
 * the mutation logic are real — once signal delivery learns the
 * mask, behavior comes online without touching this file.
 *
 * sigprocmask() in single-threaded programs is functionally
 * identical to pthread_sigmask(); musl funnels its sigprocmask.o
 * straight into pthread_sigmask, so providing this symbol resolves
 * both.
 */

#include <signal.h>
#include <stddef.h>
#include <qsoe-system.h>
#include <qsoe/tls.h>

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    qsoe_tcb_t *t = qsoe_curthr();

    /* Hand the caller a copy of the current mask before mutating. */
    if (oldset) {
        unsigned char *dst = (unsigned char *)oldset;
        for (unsigned i = 0; i < sizeof t->sig_mask; ++i) {
            dst[i] = t->sig_mask[i];
        }
    }

    if (!set) return 0;   /* query-only path */

    const unsigned char *src = (const unsigned char *)set;
    switch (how) {
    case SIG_BLOCK:
        for (unsigned i = 0; i < sizeof t->sig_mask; ++i) {
            t->sig_mask[i] |= src[i];
        }
        return 0;
    case SIG_UNBLOCK:
        for (unsigned i = 0; i < sizeof t->sig_mask; ++i) {
            t->sig_mask[i] &= (unsigned char)~src[i];
        }
        return 0;
    case SIG_SETMASK:
        for (unsigned i = 0; i < sizeof t->sig_mask; ++i) {
            t->sig_mask[i] = src[i];
        }
        return 0;
    default:
        return EINVAL;
    }
}
