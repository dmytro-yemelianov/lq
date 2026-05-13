/*
 * <qsoe/tls.h> — per-thread state via the RISC-V tp register.
 *
 * v0.4: introduce per-thread state for the Thread* API. The RISC-V
 * `tp` register holds a pointer to this thread's qsoe_tcb_t. The
 * crt0 of every process plants the address of qsoe_main_tcb into tp
 * for the main thread; ThreadCreate allocates a fresh qsoe_tcb_t and
 * writes its address into the new TCB's tp at startup.
 *
 * All previously-global per-process state (qsoe_errno, qsoe_ipcbuf,
 * qsoe_self_pid) now lives in this struct and is reached via macros
 * that expand through qsoe_curthr().
 *
 * Stored sizes pun seL4_CPtr / pointer-typed seL4 caps as unsigned
 * long so this header doesn't have to pull in <sel4_types.h>. Callers
 * cast at use.
 */
#ifndef QSOE_TLS_H
#define QSOE_TLS_H

typedef int           pid_t;  /* duplicated from qrv.h to keep tls.h standalone */

typedef struct qsoe_tcb {
    int        tid;             /* 1 for the main thread of every process */
    int        qerrno;          /* per-thread errno */
    int        cancel_pending;  /* set by ThreadCancel, polled at lib entry */
    int        detached;        /* ThreadDetach flag */
    int        exited;          /* trampoline sets this on return from func */
    int        reaped;          /* Join/Detach claimed the exit_status */

    pid_t      self_pid;        /* QSOE_PID_TASKMAN for taskman, else process pid */
    void      *ipcbuf;          /* seL4_IPCBuffer * for this thread */

    /* Threading-management state (filled by ThreadCreate / taskman). */
    unsigned long tcb_cap;      /* seL4 CPtr to this thread's TCB */
    unsigned long join_ntfn;    /* Notification CPtr; 0 if detached or main */
    void      *exit_status;     /* what func returned; read by ThreadJoin */

    /* ThreadCtl-managed. */
    char       name[16];
    unsigned   runmask;
} qsoe_tcb_t;

/* The main thread of every process has static storage so the crt0 can
 * compute its address. Defined in libqsoe/src/state.c. */
extern qsoe_tcb_t qsoe_main_tcb;

/* Return the current thread's qsoe_tcb_t pointer (held in tp). */
static inline qsoe_tcb_t *qsoe_curthr(void)
{
    qsoe_tcb_t *t;
    __asm__("mv %0, tp" : "=r"(t));
    return t;
}

/*
 * Spinlock primitive. RISC-V atomics (the `a` extension in our
 * -march=rv64imac_zicsr_zifencei) compile this down to an acquire-
 * ordered amoswap.w plus a release-ordered sw. Used by libqsoe's
 * state.c to protect per-process pools against concurrent access
 * from worker threads on other harts under SMP.
 */
typedef volatile unsigned qsoe_spinlock_t;

static inline void qsoe_spin_lock(qsoe_spinlock_t *l)
{
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        ; /* spin until predecessor releases */
}

static inline void qsoe_spin_unlock(qsoe_spinlock_t *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

/* Public surface — same names as the v0.3 globals, now macros over tp.
 * qsoe_errno and qsoe_self_pid are lvalues; qsoe_ipcbuf is r-value
 * only (the cast loses lvalue-ness — write through qsoe_curthr()
 * directly on the rare init path that needs it). */
#define qsoe_errno     (qsoe_curthr()->qerrno)
#define qsoe_self_pid  (qsoe_curthr()->self_pid)
#define qsoe_ipcbuf    ((seL4_IPCBuffer *)qsoe_curthr()->ipcbuf)

#endif /* QSOE_TLS_H */
