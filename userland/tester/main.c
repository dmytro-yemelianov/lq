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

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "../taskman/sel4_types.h"
#include "../taskman/qsoe_invoke.h"
#include <qsoe-system.h>
#include "../libqsoe/include/qsoe/slots.h"
#include "../libqsoe/include/qsoe/wire.h"
#include <sys/sync.h>

/* Compatibility shims for the few call sites that compose hex / decimal
 * via short helpers.  Implemented on top of printf so every byte tester
 * emits goes through libc's stdio path.  The %016lx / %d format strings
 * match the historical output exactly so existing log captures stay
 * comparable. */
static inline void puthex(unsigned long x) { printf("0x%016lx", x); }
static inline void putd  (int v)           { printf("%d", v);       }

int main(int argc, char **argv, char **envp)
{
    /* /dev/console is not a tty (no isatty bit), so musl defaults
     * stdout to fully-buffered.  Switch to unbuffered so each printf()
     * write hits fd 1 directly — multi-threaded sections (joinable
     * worker, detached worker, SMP spawn) write to the same stdout
     * concurrently and line-buffered mode produced cross-thread
     * doubling.  Unbuffered keeps each putc/printf write atomic. */
    setvbuf(stdout, 0, _IONBF, 0);

    printf("[tester] alive, pid=");
    putd((int)qsoe_self_pid);
    printf(" argc=");
    putd(argc);
    if (argc > 0 && argv[0]) {
        printf(" argv[0]=%s", argv[0]);
    }
    if (envp && envp[0]) {
        printf(" envp[0]=%s", envp[0]);
    }
    putchar('\n');

    /* --- 0. v0.5.0 stdio smoke-test: write through fds 1 and 2,
     *        then open /dev/console explicitly and write through
     *        the new fd. These exercise the full pathmgr + console
     *        resmgr path without going through musl yet. --- */
    {
        const char *out_msg = "[tester] stdout via write(1)\n";
        const char *err_msg = "[tester] stderr via write(2)\n";
        unsigned out_len = 0; while (out_msg[out_len]) out_len++;
        unsigned err_len = 0; while (err_msg[err_len]) err_len++;
        write(1, out_msg, out_len);
        write(2, err_msg, err_len);

        int cfd = open("/dev/console", 0);
        printf("[tester] open(/dev/console) -> fd=");
        putd(cfd);
        putchar('\n');
        if (cfd >= 0) {
            const char *m = "[tester] write via opened /dev/console\n";
            unsigned ml = 0; while (m[ml]) ml++;
            long w = write(cfd, m, ml);
            printf("[tester] write -> ");
            putd((int)w);
            putchar('\n');
            close(cfd);
        }
    }


    /* --- 1. Round-trip on SYSMGR_COID (no ConnectAttach needed) --- */
    for (int i = 1; i <= 3; ++i) {
        unsigned long payload = (unsigned long)i;
        unsigned long reply   = 0;
        int rc = MsgSend(SYSMGR_COID, &payload, sizeof payload,
                          &reply, sizeof reply);
        if (rc < 0) {
            printf("[tester] MsgSend FAILED\n");
            break;
        }
        printf("[tester] MsgSend(");
        putd(i);
        printf(") on SYSMGR_COID -> ");
        putd((int)reply);
        putchar('\n');
    }

    /* --- 2. ConnectServerInfo --- */
    struct _server_info si;
    int sirc = ConnectServerInfo(0, SYSMGR_COID, &si);
    if (sirc == 0) {
        printf("[tester] ConnectServerInfo: pid=");
        putd(si.pid);
        printf(" chid=");
        putd(si.chid);
        printf(" scoid=");
        putd(si.scoid);
        putchar('\n');
    } else {
        printf("[tester] ConnectServerInfo FAILED errno=");
        putd(qsoe_errno);
        putchar('\n');
    }

    /* --- 3. ConnectFlags: query, set CLOEXEC, query again --- */
    int q0 = ConnectFlags(0, SYSMGR_COID, 0, 0);
    int q1 = ConnectFlags(0, SYSMGR_COID, QSOE_COF_CLOEXEC, QSOE_COF_CLOEXEC);
    int q2 = ConnectFlags(0, SYSMGR_COID, 0, 0);
    printf("[tester] ConnectFlags: pre=");
    puthex((unsigned long)q0);
    printf(" prev_at_set=");
    puthex((unsigned long)q1);
    printf(" post=");
    puthex((unsigned long)q2);
    putchar('\n');

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
        printf("[tester] PING_CLIENTINFO: echo=");
        putd((int)mr0);
        printf(" taskman_saw_pid=");
        putd((int)mr1);
        putchar('\n');
    }

    /* --- 5a. ThreadCreate + ThreadJoin: joinable worker. --- */
    {
        extern void *worker_fn(void *);
        int tid = ThreadCreate(0, worker_fn, (void *)0xABCDUL, 0);
        printf("[tester] ThreadCreate(joinable) -> tid=");
        putd(tid);
        putchar('\n');
        void *status = 0;
        int jr = ThreadJoin(tid, &status);
        printf("[tester] ThreadJoin -> rc=");
        putd(jr);
        printf(" status=");
        puthex((unsigned long)status);
        putchar('\n');
    }

    /* --- 5b. Detached worker: spawn-and-forget; yield until it
     *        announces itself. ThreadJoin on a detached thread would
     *        fail with EINVAL — we don't try. --- */
    {
        extern void *worker_detached_fn(void *);
        struct _thread_attr attr = { .flags = QSOE_PTHREAD_CREATE_DETACHED,
                                      .prio  = 254 };
        int tid = ThreadCreate(0, worker_detached_fn, 0, &attr);
        printf("[tester] ThreadCreate(detached) -> tid=");
        putd(tid);
        putchar('\n');
        for (int i = 0; i < 8; ++i) qsoe_sys_yield();
    }

    /* --- 5c. ThreadCancel: spawn a worker that loops over MsgSends,
     *        cancel it, then ThreadJoin observes the cancel status. --- */
    {
        extern void *worker_loop_fn(void *);
        int tid = ThreadCreate(0, worker_loop_fn, 0, 0);
        printf("[tester] ThreadCreate(loop) -> tid=");
        putd(tid);
        putchar('\n');
        for (int i = 0; i < 4; ++i) qsoe_sys_yield();
        int cr = ThreadCancel(tid, 0);
        printf("[tester] ThreadCancel -> rc=");
        putd(cr);
        putchar('\n');
        void *status = 0;
        int jr = ThreadJoin(tid, &status);
        printf("[tester] ThreadJoin -> rc=");
        putd(jr);
        printf(" status=");
        puthex((unsigned long)status);
        putchar('\n');
    }

    /* --- 6. ThreadCtl(NAME). --- */
    {
        int rc = ThreadCtl(QSOE_TCTL_NAME, (void *)"main");
        printf("[tester] ThreadCtl(NAME) -> rc=");
        putd(rc);
        putchar('\n');
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
            printf("[tester] SMP ThreadCreate(cpu=");
            putd(cpu);
            printf(") -> tid=");
            putd(tids[k]);
            putchar('\n');
        }
        for (int k = 0; k < 3; ++k) {
            if (tids[k] > 0) {
                void *status = 0;
                ThreadJoin(tids[k], &status);
                printf("[tester] SMP join tid=");
                putd(tids[k]);
                printf(" status=");
                puthex((unsigned long)status);
                putchar('\n');
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
        printf("[tester] pulse chid=");
        puthex((unsigned long)pulse_chid);
        putchar('\n');

        int pulse_coid = ConnectAttach(ND_LOCAL_NODE, qsoe_self_pid,
                                        pulse_chid, 0, 0);
        printf("[tester] pulse coid=");
        puthex((unsigned long)pulse_coid);
        putchar('\n');

        extern void *worker_pulse_sender_fn(void *);
        struct _thread_attr at = { 0 };
        at.runmask = 0x2;  /* hart 1 — runs in parallel with main on hart 0 */
        int sender_tid = ThreadCreate(0, worker_pulse_sender_fn,
                                       (void *)(long)pulse_coid, &at);
        printf("[tester] pulse sender tid=");
        putd(sender_tid);
        putchar('\n');

        for (int i = 0; i < 3; ++i) {
            struct _pulse p;
            struct _msg_info mi;
            int rcv = MsgReceive(pulse_chid, &p, sizeof p, &mi);
            printf("[tester] MsgReceive pulse rcv=");
            putd(rcv);
            printf(" flags=");
            puthex((unsigned long)mi.flags);
            printf(" code=");
            putd(p.code);
            printf(" value=");
            putd(p.value.sival_int);
            printf(" scoid=");
            putd(p.scoid);
            putchar('\n');
        }

        void *st = 0;
        ThreadJoin(sender_tid, &st);

        ConnectDetach(pulse_coid);
        ChannelDestroy(pulse_chid);
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

        printf("[cap-leak] 100 Channel cycles: s_next_slot before=");
        putd((int)before);
        printf(" after=");
        putd((int)after);
        printf(" delta=");
        putd((int)(after - before));
        if (failed) printf(" (LOOP FAILED)");
        putchar('\n');
    }

    /* v0.8-rc1: slogf smoke test. */
    {
        #include <sys/slog.h>
        #include <sys/slogcodes.h>
        printf("[tester] slogf smoke: writing 3 events\n");
        slogf(_SLOGC_TEST, _SLOG_INFO,    "hello from tester pid %d", (int)qsoe_self_pid);
        slogf(_SLOGC_TEST, _SLOG_WARNING, "this is a warning at counter %d", 42);
        slogf(_SLOGC_TEST, _SLOG_DEBUG1,  "debug payload");
        printf("[tester]   3 events sent\n");
    }

    /* v0.8-rc1: rsrcdb smoke test.  Boot seeded MEMORY entries from
     * the FDT (via syscfg); query a few back, then create an IRQ
     * range, attach a single IRQ, detach it, destroy the range. */
    {
        #include <sys/rsrcdbmgr.h>
        printf("[tester] rsrcdb smoke: query MEMORY\n");
        rsrc_alloc_t got[4];
        int n = rsrcdbmgr_query(got, 4, 0, RSRCDBMGR_MEMORY);
        printf("[tester]   MEMORY entries: ");
        putd(n);
        putchar('\n');
        for (int i = 0; i < n && i < 4; ++i) {
            printf("[tester]   [");
            puthex(got[i].start);
            printf("..");
            puthex(got[i].end);
            printf("] flags=");
            puthex(got[i].flags);
            putchar('\n');
        }
        printf("[tester] rsrcdb smoke: create IRQ pool 64..71\n");
        rsrc_alloc_t mk = { 64, 71, RSRCDBMGR_IRQ, 0 };
        int rc = rsrcdbmgr_create(&mk, 1);
        printf("[tester]   create rc=");
        putd(rc);
        putchar('\n');

        printf("[tester] rsrcdb smoke: attach 1 IRQ\n");
        rsrc_request_t req = { 0 };
        req.length = 1;
        req.flags  = RSRCDBMGR_IRQ;
        rc = rsrcdbmgr_attach(&req, 1);
        printf("[tester]   attach rc=");
        putd(rc);
        if (rc == 0) {
            printf(" granted=");
            puthex(req.start);
        }
        putchar('\n');

        if (rc == 0) {
            printf("[tester] rsrcdb smoke: detach\n");
            rc = rsrcdbmgr_detach(&req, 1);
            printf("[tester]   detach rc=");
            putd(rc);
            putchar('\n');
        }
    }

    /* v0.8: MAP_PHYS smoke test.  Use the unused device-UT at
     * 0x04000000 (sb=26, 64 MiB; sits between CLINT and PLIC on
     * qemu-virt and isn't bound to any actual device — perfect for
     * a non-destructive smoke of the mapping path).  We only check
     * that the mmap succeeds; don't read the region (the underlying
     * bus has no device there, so a load could behave unpredictably). */
    {
        printf("[tester] MAP_PHYS smoke @ 0x04000000\n");
        void *p = qsoe_mmap(0, 0x1000, 0,
                            QSOE_MAP_PHYS, -1, 0x04000000UL);
        if (p == QSOE_MAP_FAILED) {
            printf("[tester]   FAIL: errno=");
            putd(qsoe_errno);
            putchar('\n');
        } else {
            printf("[tester]   mapped at ");
            puthex((unsigned long)p);
            putchar('\n');
        }
    }

    /* v0.8-rc2: PCI smoke via libpci.  Open /dev/pci, list devices,
     * attach one, read its BARs and IRQ, detach.  The pci-server lives
     * at /sbin/pci-server and is brought up by init.sh before us. */
    {
        #include <pci/pci.h>
        printf("[tester] pci_smoke: pci_bios_present\n");
        uint32_t lastbus = 0, version = 0;
        if (pci_bios_present(&lastbus, &version) == 0) {
            printf("[tester]   lastbus=");
            putd((int)lastbus);
            printf(" version=");
            putd((int)version);
            putchar('\n');
        } else {
            printf("[tester]   FAIL: pci_bios_present errno=");
            putd(qsoe_errno);
            putchar('\n');
        }

        printf("[tester] pci_smoke: enumerate (any vid)\n");
        for (uint32_t idx = 0; idx < 16; ++idx) {
            pci_bdf_t bdf = pci_device_find(0xffff, 0xffff, 0, idx);
            if (bdf == PCI_BDF_NONE) break;
            uint16_t vid = 0, did = 0;
            uint32_t ccode = 0;
            pci_device_read_vid  (bdf, &vid);
            pci_device_read_did  (bdf, &did);
            pci_device_read_ccode(bdf, &ccode);
            printf("[tester]   [");
            putd((int)idx);
            printf("] bdf=");
            puthex(bdf);
            printf(" vid:did=");
            puthex(vid);
            putchar(':');
            puthex(did);
            printf(" ccode=");
            puthex(ccode >> 8);
            putchar('\n');

            /* Attach + read BARs + IRQ + detach. */
            pci_devhdl_t hdl = 0;
            if (pci_device_attach(bdf, 0, &hdl) == 0) {
                uint32_t nba = 0;
                pci_ba_t ba[6];
                pci_device_read_ba(hdl, &nba, ba);
                uint32_t irq = 0;
                (void)pci_device_read_irq(hdl, &irq);
                printf("[tester]       nba=");
                putd((int)nba);
                printf(" irq=");
                putd((int)irq);
                putchar('\n');
                for (uint32_t i = 0; i < nba; ++i) {
                    printf("[tester]       BAR");
                    putd((int)ba[i].bar_num);
                    printf(" type=");
                    putd((int)ba[i].type);
                    printf(" addr=");
                    puthex(ba[i].addr);
                    putchar('\n');
                }
                pci_device_detach(hdl);
            } else {
                printf("[tester]       attach FAIL errno=");
                putd(qsoe_errno);
                putchar('\n');
            }
        }
    }

    /* v0.8: Sync* primitives smoke test.
     *   1. SyncMutex: lock / unlock from one thread; recursive
     *      acquire by the same thread.
     *   2. SyncSem: post / wait pair across two threads (background
     *      worker waits, main posts).
     *   3. SyncCondvar: worker blocks on cond, main signals.
     */
    {
        /* --- mutex --- */
        printf("[tester] sync_smoke: mutex\n");
        sync_t mx = QRV_SYNC_INITIALIZER;
        SyncTypeCreate(QRV_SYNC_MUTEX_FREE, &mx, 0);

        int rc = SyncMutexLock(&mx);
        printf("[tester]   lock rc=");
        putd(rc);
        printf(" owner=");
        puthex(mx.owner);
        putchar('\n');

        /* Recursive re-acquire by the same thread. */
        rc = SyncMutexLock(&mx);
        printf("[tester]   lock(recursive) rc=");
        putd(rc);
        printf(" count=");
        putd((int)mx.count);
        putchar('\n');

        rc = SyncMutexUnlock(&mx);
        printf("[tester]   unlock(inner) rc=");
        putd(rc);
        printf(" count=");
        putd((int)mx.count);
        putchar('\n');

        rc = SyncMutexUnlock(&mx);
        printf("[tester]   unlock(final) rc=");
        putd(rc);
        printf(" owner=");
        puthex(mx.owner);
        putchar('\n');

        /* --- semaphore: post-then-wait (no blocking expected). --- */
        printf("[tester] sync_smoke: sem post-then-wait\n");
        sync_t sm = QRV_SYNC_INITIALIZER;
        SyncTypeCreate(QRV_SYNC_SEM, &sm, 0);

        SyncSemPost(&sm);
        printf("[tester]   post  count=");
        putd((int)sm.count);
        putchar('\n');

        rc = SyncSemWait(&sm, 0);
        printf("[tester]   wait  rc=");
        putd(rc);
        printf(" count=");
        putd((int)sm.count);
        putchar('\n');

        /* --- semaphore: wait-then-post across two threads.  Worker
         *     blocks on the sem; main delays a touch (yield), then
         *     posts.  Worker should wake and print. */
        printf("[tester] sync_smoke: sem cross-thread\n");
        extern void *sync_sem_waiter_fn(void *);
        int tid = ThreadCreate(0, sync_sem_waiter_fn, &sm, 0);
        for (int i = 0; i < 4; ++i) qsoe_sys_yield();
        printf("[tester]   posting after worker has parked\n");
        SyncSemPost(&sm);
        void *st = 0;
        ThreadJoin(tid, &st);
        printf("[tester]   joined worker status=");
        puthex((unsigned long)st);
        putchar('\n');

        /* --- condvar: worker waits, main signals. --- */
        printf("[tester] sync_smoke: condvar\n");
        sync_t cmx = QRV_SYNC_INITIALIZER;
        sync_t cnd = QRV_SYNC_INITIALIZER;
        SyncTypeCreate(QRV_SYNC_MUTEX_FREE, &cmx, 0);
        SyncTypeCreate(QRV_SYNC_COND,       &cnd, 0);

        /* Worker takes the mutex, waits on cond.  Main yields a few
         * times so the worker reliably parks, then signals. */
        sync_t *pair[2] = { &cmx, &cnd };
        extern void *sync_cond_waiter_fn(void *);
        int ctid = ThreadCreate(0, sync_cond_waiter_fn, pair, 0);
        for (int i = 0; i < 6; ++i) qsoe_sys_yield();
        printf("[tester]   signaling cond\n");
        SyncMutexLock(&cmx);
        SyncCondvarSignal(&cnd, 0);
        SyncMutexUnlock(&cmx);
        ThreadJoin(ctid, &st);
        printf("[tester]   joined cond-waiter status=");
        puthex((unsigned long)st);
        putchar('\n');
    }

    printf("[tester] done, returning 0 (→ _exit via crt0)\n");
    return 0;
}

/* Worker entry. Lives in tester's .text — the new thread shares the
 * VSpace, so the trampoline transitions cleanly into here. */
void *worker_fn(void *arg)
{
    printf("[worker] alive, tid=");
    putd(qsoe_curthr()->tid);
    printf(" arg=");
    puthex((unsigned long)arg);
    putchar('\n');
    return (void *)0xC0FFEE01UL;
}

void *worker_detached_fn(void *arg)
{
    (void)arg;
    printf("[detached worker] alive, tid=");
    putd(qsoe_curthr()->tid);
    putchar('\n');
    return (void *)0xDEADBEEFUL;  /* nobody reads this */
}

void *worker_loop_fn(void *arg)
{
    (void)arg;
    printf("[loop worker] starting MsgSend loop, tid=");
    putd(qsoe_curthr()->tid);
    putchar('\n');
    /* Many MsgSends — each hits the cancel point. Without cancel, we
     * spin here forever; with cancel, the next MsgSend self-destructs. */
    for (int i = 0; i < 1000; ++i) {
        unsigned long payload = (unsigned long)i;
        unsigned long reply   = 0;
        MsgSend(SYSMGR_COID, &payload, sizeof payload,
                 &reply, sizeof reply);
        qsoe_sys_yield();
    }
    printf("[loop worker] finished without cancel?!\n");
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
        printf("[sender tid=");
        putd(qsoe_curthr()->tid);
        printf("] MsgSendPulse code=");
        putd(i + 1);
        printf(" rc=");
        putd(rc);
        putchar('\n');
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
        printf("[smp tid=");
        putd(qsoe_curthr()->tid);
        printf(" cpu=");
        putd((int)cpu);
        printf("] reply=");
        putd((int)reply);
        putchar('\n');
    }
    return (void *)(0xCAFE0000UL | cpu);
}

/* Sync* workers (referenced by extern in the smoke block in main). */
void *sync_sem_waiter_fn(void *arg)
{
    sync_t *sem = (sync_t *)arg;
    printf("[sem-waiter tid=");
    putd(qsoe_curthr()->tid);
    printf("] parking on sem (count=");
    putd((int)sem->count);
    printf(")\n");
    int rc = SyncSemWait(sem, 0);
    printf("[sem-waiter tid=");
    putd(qsoe_curthr()->tid);
    printf("] woke rc=");
    putd(rc);
    printf(" count=");
    putd((int)sem->count);
    putchar('\n');
    return (void *)0x5E110001UL;
}

void *sync_cond_waiter_fn(void *arg)
{
    sync_t **pair = (sync_t **)arg;
    sync_t *mx  = pair[0];
    sync_t *cnd = pair[1];
    SyncMutexLock(mx);
    printf("[cond-waiter tid=");
    putd(qsoe_curthr()->tid);
    printf("] mutex held, calling SyncCondvarWait\n");
    int rc = SyncCondvarWait(cnd, mx);
    printf("[cond-waiter tid=");
    putd(qsoe_curthr()->tid);
    printf("] woke rc=");
    putd(rc);
    putchar('\n');
    SyncMutexUnlock(mx);
    return (void *)0xC0DEC0DEUL;
}
