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

/* libqsoe globals — msg.c declares them; tester provides storage for
 * qsoe_ipcbuf since the taskman build does it from main(). */
seL4_IPCBuffer *qsoe_ipcbuf;

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

    sel4_debug_puts("[tester] done\n");
    for (;;) __asm__ volatile("nop");
    return 0;
}
