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

    /* --- 7a. Pulses (v0.4.2 + v0.4.3 bound-Notification wake). Create a
     *         side-channel, self-connect to it, spawn a sender thread on
     *         hart 1, and MsgReceive on hart 0. The sender's busy-spin
     *         delay ensures the first MsgReceive actually blocks — its
     *         wake is delivered by the bound Notification kernel-side
     *         when the sender's MsgSendPulse signals it. Subsequent
     *         receives may find pulses already queued (faster path). --- */
    {
        int pulse_chid = ChannelCreate(QSOE_SIDE_CHANNEL);
        sel4_debug_puts("[tester] pulse chid=");
        puthex((unsigned long)pulse_chid);
        sel4_debug_putchar('\n');

        int pulse_coid = ConnectAttach(ND_LOCAL_NODE, qsoe_self_pid,
                                        pulse_chid, 0, 0);
        sel4_debug_puts("[tester] pulse coid=");
        puthex((unsigned long)pulse_coid);
        sel4_debug_putchar('\n');

        extern void *worker_pulse_sender_fn(void *);
        struct _thread_attr at = { 0 };
        at.runmask = 0x2;  /* hart 1 — runs in parallel with main on hart 0 */
        int sender_tid = ThreadCreate(0, worker_pulse_sender_fn,
                                       (void *)(long)pulse_coid, &at);
        sel4_debug_puts("[tester] pulse sender tid=");
        putd(sender_tid);
        sel4_debug_putchar('\n');

        for (int i = 0; i < 3; ++i) {
            struct _pulse p;
            struct _msg_info mi;
            int rcv = MsgReceive(pulse_chid, &p, sizeof p, &mi);
            sel4_debug_puts("[tester] MsgReceive pulse rcv=");
            putd(rcv);
            sel4_debug_puts(" flags=");
            puthex((unsigned long)mi.flags);
            sel4_debug_puts(" code=");
            putd(p.code);
            sel4_debug_puts(" value=");
            putd(p.value.sival_int);
            sel4_debug_puts(" scoid=");
            putd(p.scoid);
            sel4_debug_putchar('\n');
        }

        void *st = 0;
        ThreadJoin(sender_tid, &st);

        ConnectDetach(pulse_coid);
        ChannelDestroy(pulse_chid);
    }

    /* --- 7b. posix_spawn hello.elf as a tiny IPC server (v0.4.3).
     *         hello now does ChannelCreate + MsgReceive+Reply loop.
     *         We ConnectAttach to hello's chid=1 with retry (it may
     *         not have created the channel yet — yields let it run);
     *         then MsgSend 3 round-trips. ConnectServerInfo confirms
     *         the server identity (pid != taskman). --- */
    {
        pid_t hpid = 0;
        int rc = posix_spawn(&hpid, "hello.elf", 0, 0, 0, 0);
        sel4_debug_puts("[tester] posix_spawn(hello.elf) -> rc=");
        putd(rc);
        sel4_debug_puts(" pid=");
        putd((int)hpid);
        sel4_debug_putchar('\n');

        if (rc == 0) {
            /* Give hello a few ticks to reach ChannelCreate. */
            for (int i = 0; i < 4; ++i) qsoe_sys_yield();

            int hcoid = -1;
            for (int try = 0; try < 8 && hcoid < 0; ++try) {
                hcoid = ConnectAttach(ND_LOCAL_NODE, hpid, /*chid=*/1, 0, 0);
                if (hcoid < 0) qsoe_sys_yield();
            }
            sel4_debug_puts("[tester] ConnectAttach(hello) -> coid=");
            putd(hcoid);
            sel4_debug_putchar('\n');

            if (hcoid >= 0) {
                /* Introspect: confirm we're really connected to hello. */
                struct _server_info si;
                int sirc = ConnectServerInfo(0, hcoid, &si);
                sel4_debug_puts("[tester] hello ConnectServerInfo: rc=");
                putd(sirc);
                sel4_debug_puts(" pid=");
                putd((int)si.pid);
                sel4_debug_puts(" chid=");
                putd(si.chid);
                sel4_debug_putchar('\n');

                for (int i = 0; i < 3; ++i) {
                    unsigned long payload = 1000UL + (unsigned long)i;
                    unsigned long reply = 0;
                    int mr = MsgSend(hcoid, &payload, sizeof payload,
                                      &reply, sizeof reply);
                    sel4_debug_puts("[tester] MsgSend(hello, ");
                    putd((int)payload);
                    sel4_debug_puts(") rc=");
                    putd(mr);
                    sel4_debug_puts(" reply=");
                    putd((int)reply);
                    sel4_debug_putchar('\n');
                }

                ConnectDetach(hcoid);
            }

            /* Yield until hello finishes its loop and exits. */
            for (int i = 0; i < 8; ++i) qsoe_sys_yield();
        }
    }

    /* --- 8. Cap-leak smoke test. With taskman's slot free-list, the
     *        SAME endpoint slot gets reused for every iteration after
     *        the first. Warm-up: do one cycle so the free list isn't
     *        empty, then measure 100 more. Expected delta=0. --- */
    {
        int warm = ChannelCreate(0);
        if (warm >= 0) ChannelDestroy(warm);

        seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
        seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_DEBUG_SLOT_COUNT,
                                                       0, 0, 0);
        qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag, &mr0, &mr1, &mr2, &mr3);
        unsigned long before = (unsigned long)mr0;

        int failed = 0;
        for (int i = 0; i < 100; ++i) {
            int chid = ChannelCreate(0);
            if (chid < 0) { failed = 1; break; }
            int rc = ChannelDestroy(chid);
            if (rc < 0) { failed = 1; break; }
        }

        mr0 = 0; mr1 = 0; mr2 = 0; mr3 = 0;
        tag = seL4_MessageInfo_new(TM_REQ_DEBUG_SLOT_COUNT, 0, 0, 0);
        qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag, &mr0, &mr1, &mr2, &mr3);
        unsigned long after = (unsigned long)mr0;

        sel4_debug_puts("[cap-leak] 100 Channel cycles: s_next_slot before=");
        putd((int)before);
        sel4_debug_puts(" after=");
        putd((int)after);
        sel4_debug_puts(" delta=");
        putd((int)(after - before));
        if (failed) sel4_debug_puts(" (LOOP FAILED)");
        sel4_debug_putchar('\n');
    }

    sel4_debug_puts("[tester] done, returning 0 (→ _exit via crt0)\n");
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

/* v0.4.3: pulse-sender worker. Pinned to hart 1 so it can run while
 * the main thread is parked in MsgReceive on hart 0. Brief busy-spin
 * before each send so the first MsgReceive on main actually blocks
 * (and exercises the bound-Notification wake path) rather than
 * finding a pulse already queued. */
void *worker_pulse_sender_fn(void *arg)
{
    int coid = (int)(long)arg;
    for (int i = 0; i < 3; ++i) {
        for (volatile int spin = 0; spin < 200000; ++spin) ;
        int rc = MsgSendPulse(coid, /*prio=*/10,
                              /*code=*/i + 1, /*value=*/(i + 1) * 100);
        sel4_debug_puts("[sender tid=");
        putd(qsoe_curthr()->tid);
        sel4_debug_puts("] MsgSendPulse code=");
        putd(i + 1);
        sel4_debug_puts(" rc=");
        putd(rc);
        sel4_debug_putchar('\n');
    }
    return (void *)0;
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
