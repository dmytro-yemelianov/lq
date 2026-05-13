/*
 * tester — v0.3.3: exercise side-channel coid + introspection API.
 *
 *   1. Speak to taskman over the pre-bound SYSMGR_COID (no
 *      ConnectAttach needed — the cap was minted at spawn time).
 *   2. ConnectServerInfo(SYSMGR_COID) — verify pid=1 chid=1.
 *   3. ConnectFlags get/set — flip COF_CLOEXEC and read it back.
 *   4. TM_REQ_PING_CLIENTINFO — taskman calls ConnectClientInfo
 *      server-side; we verify it returns our pid.
 */

#include "../taskman/sel4_syscalls.h"
#include "../taskman/sel4_types.h"
#include "../taskman/qsoe_invoke.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"
#include "../libqsoe/include/qsoe/wire.h"

/* Spawn convention from spawn.c: the kernel maps the IPC buffer at
 * this virtual address in the child's VSpace. */
#define TESTER_IPC_BUFFER ((seL4_IPCBuffer *)0x1FE000UL)

static void puthex(unsigned long x)
{
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 15; i >= 0; --i) {
        unsigned d = x & 0xF;
        buf[2 + i] = d < 10 ? '0' + d : 'a' + (d - 10);
        x >>= 4;
    }
    buf[18] = 0;
    sel4_debug_puts(buf);
}

static void putd(int v)
{
    char buf[12];
    int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = '0' + (v % 10); v /= 10; }
    if (neg) buf[n++] = '-';
    for (int i = n - 1; i >= 0; --i) sel4_debug_putchar(buf[i]);
}

int main(pid_t pid)
{
    qsoe_libqsoe_init(TESTER_IPC_BUFFER, pid);

    sel4_debug_puts("[tester] alive, pid=");
    putd(pid);
    sel4_debug_putchar('\n');

    /* --- 1. Round-trip on SYSMGR_COID (no ConnectAttach needed) --- */
    for (int i = 1; i <= 3; ++i) {
        unsigned long payload = (unsigned long)i;
        unsigned long reply   = 0;
        int rc = MsgSend(SYSMGR_COID, &payload, sizeof payload,
                          &reply, sizeof reply);
        if (rc < 0) {
            sel4_debug_puts("[tester] MsgSend FAILED\n");
            break;
        }
        sel4_debug_puts("[tester] MsgSend(");
        putd(i);
        sel4_debug_puts(") on SYSMGR_COID -> ");
        putd((int)reply);
        sel4_debug_putchar('\n');
    }

    /* --- 2. ConnectServerInfo --- */
    struct _server_info si;
    int sirc = ConnectServerInfo(0, SYSMGR_COID, &si);
    if (sirc == 0) {
        sel4_debug_puts("[tester] ConnectServerInfo: pid=");
        putd(si.pid);
        sel4_debug_puts(" chid=");
        putd(si.chid);
        sel4_debug_puts(" scoid=");
        putd(si.scoid);
        sel4_debug_putchar('\n');
    } else {
        sel4_debug_puts("[tester] ConnectServerInfo FAILED errno=");
        putd(qsoe_errno);
        sel4_debug_putchar('\n');
    }

    /* --- 3. ConnectFlags: query, set CLOEXEC, query again --- */
    int q0 = ConnectFlags(0, SYSMGR_COID, 0, 0);
    int q1 = ConnectFlags(0, SYSMGR_COID, QSOE_COF_CLOEXEC, QSOE_COF_CLOEXEC);
    int q2 = ConnectFlags(0, SYSMGR_COID, 0, 0);
    sel4_debug_puts("[tester] ConnectFlags: pre=");
    puthex((unsigned long)q0);
    sel4_debug_puts(" prev_at_set=");
    puthex((unsigned long)q1);
    sel4_debug_puts(" post=");
    puthex((unsigned long)q2);
    sel4_debug_putchar('\n');

    /* --- 4. TM_REQ_PING_CLIENTINFO: taskman's server-side
     *       ConnectClientInfo path. We pack the wire-protocol label
     *       directly via a raw seL4_Call wrapper — MsgSend doesn't
     *       expose labels. Cheating one layer below the QNX API here
     *       only because PING is a taskman-internal demo handler,
     *       not user-facing. --- */
    {
        seL4_Word mr0 = 7;  /* payload to be echoed +1 */
        seL4_Word mr1 = 0, mr2 = 0, mr3 = 0;
        seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PING_CLIENTINFO,
                                                       0, 0, 1);
        seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                                  &mr0, &mr1, &mr2, &mr3);
        (void)reply;
        sel4_debug_puts("[tester] PING_CLIENTINFO: echo=");
        putd((int)mr0);
        sel4_debug_puts(" taskman_saw_pid=");
        putd((int)mr1);
        sel4_debug_putchar('\n');
    }

    /* --- 5a. ThreadCreate + ThreadJoin: joinable worker. --- */
    {
        extern void *worker_fn(void *);
        int tid = ThreadCreate(0, worker_fn, (void *)0xABCDUL, 0);
        sel4_debug_puts("[tester] ThreadCreate(joinable) -> tid=");
        putd(tid);
        sel4_debug_putchar('\n');
        void *status = 0;
        int jr = ThreadJoin(tid, &status);
        sel4_debug_puts("[tester] ThreadJoin -> rc=");
        putd(jr);
        sel4_debug_puts(" status=");
        puthex((unsigned long)status);
        sel4_debug_putchar('\n');
    }

    /* --- 5b. Detached worker: spawn-and-forget; yield until it
     *        announces itself. ThreadJoin on a detached thread would
     *        fail with EINVAL — we don't try. --- */
    {
        extern void *worker_detached_fn(void *);
        struct _thread_attr attr = { .flags = QSOE_PTHREAD_CREATE_DETACHED,
                                      .prio  = 254 };
        int tid = ThreadCreate(0, worker_detached_fn, 0, &attr);
        sel4_debug_puts("[tester] ThreadCreate(detached) -> tid=");
        putd(tid);
        sel4_debug_putchar('\n');
        for (int i = 0; i < 8; ++i) qsoe_sys_yield();
    }

    /* --- 5c. ThreadCancel: spawn a worker that loops over MsgSends,
     *        cancel it, then ThreadJoin observes the cancel status. --- */
    {
        extern void *worker_loop_fn(void *);
        int tid = ThreadCreate(0, worker_loop_fn, 0, 0);
        sel4_debug_puts("[tester] ThreadCreate(loop) -> tid=");
        putd(tid);
        sel4_debug_putchar('\n');
        for (int i = 0; i < 4; ++i) qsoe_sys_yield();
        int cr = ThreadCancel(tid, 0);
        sel4_debug_puts("[tester] ThreadCancel -> rc=");
        putd(cr);
        sel4_debug_putchar('\n');
        void *status = 0;
        int jr = ThreadJoin(tid, &status);
        sel4_debug_puts("[tester] ThreadJoin -> rc=");
        putd(jr);
        sel4_debug_puts(" status=");
        puthex((unsigned long)status);
        sel4_debug_putchar('\n');
    }

    /* --- 6. ThreadCtl(NAME). --- */
    {
        int rc = ThreadCtl(QSOE_TCTL_NAME, (void *)"main");
        sel4_debug_puts("[tester] ThreadCtl(NAME) -> rc=");
        putd(rc);
        sel4_debug_putchar('\n');
    }

    /* --- 7. SMP: spawn 3 workers, one per hart 1/2/3, each does a few
     *        MsgSends then exits. The runmask bit selects the CPU. --- */
    {
        extern void *worker_smp_fn(void *);
        int tids[3];
        for (int k = 0; k < 3; ++k) {
            int cpu = k + 1;  /* harts 1, 2, 3 */
            struct _thread_attr attr = {
                .flags   = 0,
                .prio    = 254,
                .runmask = 1u << cpu,
            };
            tids[k] = ThreadCreate(0, worker_smp_fn,
                                    (void *)(unsigned long)cpu, &attr);
            sel4_debug_puts("[tester] SMP ThreadCreate(cpu=");
            putd(cpu);
            sel4_debug_puts(") -> tid=");
            putd(tids[k]);
            sel4_debug_putchar('\n');
        }
        for (int k = 0; k < 3; ++k) {
            if (tids[k] > 0) {
                void *status = 0;
                ThreadJoin(tids[k], &status);
                sel4_debug_puts("[tester] SMP join tid=");
                putd(tids[k]);
                sel4_debug_puts(" status=");
                puthex((unsigned long)status);
                sel4_debug_putchar('\n');
            }
        }
    }

    sel4_debug_puts("[tester] done\n");
    for (;;) __asm__ volatile("nop");
    return 0;
}

/* Worker entry. Lives in tester's .text — the new thread shares the
 * VSpace, so the trampoline transitions cleanly into here. */
void *worker_fn(void *arg)
{
    sel4_debug_puts("[worker] alive, tid=");
    putd(qsoe_curthr()->tid);
    sel4_debug_puts(" arg=");
    puthex((unsigned long)arg);
    sel4_debug_putchar('\n');
    return (void *)0xC0FFEE01UL;
}

void *worker_detached_fn(void *arg)
{
    (void)arg;
    sel4_debug_puts("[detached worker] alive, tid=");
    putd(qsoe_curthr()->tid);
    sel4_debug_putchar('\n');
    return (void *)0xDEADBEEFUL;  /* nobody reads this */
}

void *worker_loop_fn(void *arg)
{
    (void)arg;
    sel4_debug_puts("[loop worker] starting MsgSend loop, tid=");
    putd(qsoe_curthr()->tid);
    sel4_debug_putchar('\n');
    /* Many MsgSends — each hits the cancel point. Without cancel, we
     * spin here forever; with cancel, the next MsgSend self-destructs. */
    for (int i = 0; i < 1000; ++i) {
        unsigned long payload = (unsigned long)i;
        unsigned long reply   = 0;
        MsgSend(SYSMGR_COID, &payload, sizeof payload,
                 &reply, sizeof reply);
        qsoe_sys_yield();
    }
    sel4_debug_puts("[loop worker] finished without cancel?!\n");
    return (void *)0xBADBADUL;
}

/* SMP worker: each instance is pinned to one of harts 1..3. Sends a
 * few MsgSends to taskman and prints its tid + arg (the cpu it was
 * targeted at). Output across the 3 workers should interleave because
 * they truly run in parallel on different harts. */
void *worker_smp_fn(void *arg)
{
    unsigned long cpu = (unsigned long)arg;
    for (int i = 0; i < 3; ++i) {
        unsigned long payload = cpu * 100UL + (unsigned long)i;
        unsigned long reply   = 0;
        MsgSend(SYSMGR_COID, &payload, sizeof payload,
                 &reply, sizeof reply);
        sel4_debug_puts("[smp tid=");
        putd(qsoe_curthr()->tid);
        sel4_debug_puts(" cpu=");
        putd((int)cpu);
        sel4_debug_puts("] reply=");
        putd((int)reply);
        sel4_debug_putchar('\n');
    }
    return (void *)(0xCAFE0000UL | cpu);
}
