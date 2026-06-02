/*
 * libqsoe/src/thread.c — QNX/QRV-compatible threading on seL4 TCBs.
 *
 * Each thread is a seL4 TCB sharing the parent process's CSpace and
 * VSpace. taskman allocates the kernel objects (TCB, IPC frame, stack
 * frames, join Notification); libqsoe does the policy: stack and IPC
 * vaddr layout, per-thread state in qsoe_tcb_t, register setup,
 * trampoline, join sync.
 *
 * Per-process VSpace layout for worker threads (Design doc §4.7):
 *
 *   tid N's slot base = 0x200000 + (N-2) * 0x10000   (N >= 2)
 *
 *     base + 0xF000..0x10000   IPC buffer (4 KiB)
 *     base + 0xE000..0xF000    pad (1 page)
 *     base + 0x0000..0xE000    stack (56 KiB max; sp starts at +0xE000)
 *
 * Worker priority defaults to 254 (matches the main thread). Stacks
 * up to 14 pages (= 56 KiB) per the slot layout.
 */

#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#  include "proc/proc.h"

/* v0.4.1: worker region starts at 1 GiB — a separate L1 PT from the
 * image region. The 1 GiB image cap (see [[project-image-size-cap]])
 * guarantees they never collide. */
#define WORKER_BASE_VADDR     0x40000000UL
#define WORKER_SLOT_SIZE      0x10000UL    /* 64 KiB per worker */
#define WORKER_STACK_TOP_OFF  0xE000UL     /* sp = base + this */
#define WORKER_IPC_OFF        0xF000UL     /* IPC buffer at base + this */
#define WORKER_DEFAULT_PAGES  14           /* 56 KiB */
#define WORKER_DEFAULT_PRIO   254


int ThreadCreate(pid_t pid, void *(*func)(void *), void *arg,
                 const struct _thread_attr *attr)
{
    if (!func) { qsoe_errno = EINVAL; return -1; }
    if (pid != 0 && pid != qsoe_self_pid) { qsoe_errno = ENOSYS; return -1; }

    /* taskman is single-threaded in v0.4 — no kernel path to create a
     * worker for ourselves yet. */
    (void)arg; (void)attr;
    qsoe_errno = ENOSYS;
    return -1;
}

int ThreadDestroy(int tid, int priority, void *status)
{
    (void)priority;  /* v0.4: no priority adjustment on destroy */

    qsoe_tcb_t *t;
    int self;
    if (tid == 0 || tid == qsoe_curthr()->tid) {
        t = qsoe_curthr();
        self = 1;
    } else {
        t = qsoe_tcb_of_tid(tid);
        self = 0;
        if (!t) { qsoe_errno = ESRCH; return -1; }
    }

    /* Record exit status so a joiner can read it. */
    t->exit_status = status;
    t->exited      = 1;

    /* Signal the join notification iff this thread is joinable. (For a
     * detached thread, no one is waiting; signalling is harmless but
     * pointless.) */
    if (!t->detached && t->join_ntfn) {
        qsoe_sys_signal((seL4_CPtr)t->join_ntfn);
    }

    /* Suspend the TCB. Self-suspend never returns. */
    qsoe_tcb_suspend((seL4_CPtr)t->tcb_cap);
    if (self) {
        for (;;) __asm__ volatile("nop");  /* unreachable */
    }
    return 0;
}

int ThreadDetach(int tid)
{
    qsoe_tcb_t *t = (tid == 0) ? qsoe_curthr() : qsoe_tcb_of_tid(tid);
    if (!t) { qsoe_errno = ESRCH; return -1; }
    t->detached = 1;
    /* If the thread has already exited, mark its slot reaped now —
     * nobody will Join. The seL4 TCB cap and frames stay until
     * process-level cleanup (v0.4.1+); the slot's vaddr range stays
     * claimed (monotonic allocator). */
    if (t->exited && t != &qsoe_main_tcb) t->reaped = 1;
    return 0;
}

int ThreadJoin(int tid, void **status)
{
    qsoe_tcb_t *t = qsoe_tcb_of_tid(tid);
    if (!t) { qsoe_errno = ESRCH; return -1; }
    if (t->detached) { qsoe_errno = EINVAL; return -1; }
    if (!t->join_ntfn) { qsoe_errno = EINVAL; return -1; }

    /* Wait on the join notification. seL4_Wait is sticky: if the
     * exiting thread already signalled, the Wait returns immediately. */
    (void)qsoe_sys_wait((seL4_CPtr)t->join_ntfn);

    if (status) *status = t->exit_status;
    /* Mark the slot reaped — future lookups for this tid return NULL.
     * Resources stay allocated (v0.4.1+). */
    if (t != &qsoe_main_tcb) t->reaped = 1;
    return 0;
}

int ThreadCancel(int tid, void (*canstub)(void))
{
    (void)canstub;  /* v0.4: not invoked */
    qsoe_tcb_t *t = (tid == 0) ? qsoe_curthr() : qsoe_tcb_of_tid(tid);
    if (!t) { qsoe_errno = ESRCH; return -1; }
    t->cancel_pending = 1;
    return 0;
}

int ThreadCtl(int cmd, void *data)
{
    qsoe_tcb_t *t = qsoe_curthr();
    switch (cmd) {
    case QSOE_TCTL_NAME: {
        if (!data) { qsoe_errno = EINVAL; return -1; }
        const char *src = (const char *)data;
        for (int i = 0; i < 15; ++i) {
            t->name[i] = src[i];
            if (!src[i]) { t->name[i+1] = 0; return 0; }
        }
        t->name[15] = 0;
        return 0;
    }
    case QSOE_TCTL_RUNMASK: {
        if (!data) { qsoe_errno = EINVAL; return -1; }
        t->runmask = *(unsigned *)data;
        return 0;
    }
    case QSOE_TCTL_IO:
        /* QNX/QRV compat: x86 I/O-port privilege escalation.  On
         * RISC-V there's no privileged-port concept, and MMIO is
         * gated by VSpace mappings (which mmap(MAP_PHYS) handles).
         * So this is a no-op success. */
        return 0;
    default:
        qsoe_errno = ENOSYS;
        return -1;
    }
}
