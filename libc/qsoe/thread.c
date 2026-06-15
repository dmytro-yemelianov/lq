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
 * Worker priority defaults to the QNX default user priority (matches the
 * main thread's spawn priority); a driver raises its IST above it with
 * SchedSet.  Stacks up to 14 pages (= 56 KiB) per the slot layout.
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"


/* v0.4.1: worker region starts at 1 GiB — a separate L1 PT from the
 * image region. The 1 GiB image cap (see [[project-image-size-cap]])
 * guarantees they never collide. */
#define WORKER_BASE_VADDR     0x40000000UL
#define WORKER_SLOT_SIZE      0x10000UL    /* 64 KiB per worker */
#define WORKER_STACK_TOP_OFF  0xE000UL     /* sp = base + this */
#define WORKER_IPC_OFF        0xF000UL     /* IPC buffer at base + this */
#define WORKER_DEFAULT_PAGES  14           /* 56 KiB */
/* QNX default user-thread priority (0..255 scale, higher = higher).  Must
 * track taskman's TM_PRIO_USER_DEFAULT; both await a shared cross-kernel
 * priority-band header (the NQ-side half of the QNX 0..255 alignment). */
#define WORKER_DEFAULT_PRIO   10

/* Trampoline: every new thread enters here. We're called with C-ABI
 * argument registers a0=func, a1=arg (set by ThreadCreate via
 * WriteRegisters). On return from func, terminate via ThreadDestroy. */
__attribute__((noreturn))
static void thread_trampoline(void *(*func)(void *), void *arg)
{
    void *ret = func(arg);
    /* ThreadDestroy(0, ...) means "destroy self". It signals any
     * joiner, then suspends our own TCB — we never come back. */
    ThreadDestroy(0, 0, ret);
    for (;;) __asm__ volatile("nop");
}

int ThreadCreate(pid_t pid, void *(*func)(void *), void *arg,
                 const struct _thread_attr *attr)
{
    if (!func) { qsoe_errno = EINVAL; return -1; }
    if (pid != 0 && pid != qsoe_self_pid) { qsoe_errno = ENOSYS; return -1; }

    /* Pick parameters. */
    unsigned stack_pages = WORKER_DEFAULT_PAGES;
    unsigned prio        = WORKER_DEFAULT_PRIO;
    unsigned affinity    = 0;
    int      detached    = 0;
    if (attr) {
        if (attr->stacksize) {
            stack_pages = (unsigned)((attr->stacksize + 4095) / 4096);
            if (stack_pages == 0) stack_pages = 1;
            if (stack_pages > WORKER_DEFAULT_PAGES) stack_pages = WORKER_DEFAULT_PAGES;
        }
        if (attr->prio)  prio = (unsigned)attr->prio;
        if (attr->flags & QSOE_PTHREAD_CREATE_DETACHED) detached = 1;
        /* runmask: bitmask of permitted CPUs. v0.4 picks the lowest
         * set bit and pins to that single CPU. v0.5+ may rotate or
         * track multi-CPU eligibility. */
        if (attr->runmask) {
            unsigned r = attr->runmask;
            unsigned i = 0;
            while ((r & 1) == 0 && i < 31) { r >>= 1; ++i; }
            affinity = i;
        }
    }

    /* Allocate a per-thread state record. The index in the pool is
     * tid-2; we use it to compute the slot vaddr too. */
    qsoe_tcb_t *t = qsoe_worker_alloc();
    if (!t) { qsoe_errno = ENOMEM; return -1; }
    int local_tid = t->tid;
    unsigned long base       = WORKER_BASE_VADDR
                              + (unsigned long)(local_tid - 2) * WORKER_SLOT_SIZE;
    unsigned long stack_top  = base + WORKER_STACK_TOP_OFF;
    unsigned long ipc_vaddr  = base + WORKER_IPC_OFF;

    /* Ask taskman to retype the kernel objects + map our frames.
     * MR3 packs prio (low byte) and affinity (next byte). */
    seL4_Word mr0 = stack_top;
    seL4_Word mr1 = stack_pages;
    seL4_Word mr2 = ipc_vaddr;
    seL4_Word mr3 = (prio & 0xffu) | ((affinity & 0xffu) << 8);
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_THREAD_ALLOC, 0, 0, 4);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word werr = seL4_MessageInfo_get_label(reply);
    if (werr != 0) {
        t->tid = 0;  /* return slot */
        qsoe_errno = (int)werr;
        return -1;
    }
    seL4_CPtr tcb_slot   = (seL4_CPtr)mr0;
    seL4_CPtr ntfn_slot  = (seL4_CPtr)mr1;
    int       tid_assigned = (int)mr2;
    seL4_CPtr reply_slot = (seL4_CPtr)mr3;   /* this worker's own reply object */

    /* Populate the per-thread state. tp will be installed below via
     * WriteRegisters. */
    t->tid            = tid_assigned;
    t->qerrno         = 0;
    t->cancel_pending = 0;
    t->detached       = detached;
    t->exited         = 0;
    t->self_pid       = qsoe_self_pid;
    t->ipcbuf         = (void *)ipc_vaddr;
    t->tcb_cap        = tcb_slot;
    t->join_ntfn      = ntfn_slot;
    t->reply_cap      = reply_slot;
    t->exit_status    = 0;
    t->runmask        = 0;
    for (int i = 0; i < 16; ++i) t->name[i] = 0;

    /* WriteRegisters: pc=trampoline, sp=stack_top, tp=&new tcb,
     * a0=func, a1=arg. Caller's gp also goes through so the new
     * thread can do gp-relative addressing. */
    qsoe_user_ctx_t ctx;
    for (unsigned i = 0; i < sizeof ctx; ++i) ((char *)&ctx)[i] = 0;
    ctx.pc = (seL4_Word)(unsigned long)thread_trampoline;
    ctx.sp = stack_top;
    {
        seL4_Word gpval;
        __asm__("mv %0, gp" : "=r"(gpval));
        ctx.gp = gpval;
    }
    ctx.tp = (seL4_Word)(unsigned long)t;
    ctx.a0 = (seL4_Word)(unsigned long)func;
    ctx.a1 = (seL4_Word)(unsigned long)arg;
    seL4_Word werr2 = qsoe_tcb_write_registers(tcb_slot, /*resume*/1, &ctx);
    if (werr2 != 0) {
        t->tid = 0;
        qsoe_errno = (int)werr2;
        return -1;
    }
    /* TCB_WriteRegisters with resume_target=1 starts the thread. */
    return tid_assigned;
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
