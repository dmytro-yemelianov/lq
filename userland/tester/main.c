/*
 * tester — v0.3.2: exercise libqsoe end-to-end against taskman.
 *
 * Connects to taskman's primary channel (chid 1), sends three small
 * MsgSend rounds, prints the replies, detaches.
 */

#include "../taskman/sel4_syscalls.h"
#include "../taskman/sel4_types.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"

/* libqsoe's globals. State.c defines qsoe_errno; qsoe_ipcbuf is
 * declared in qsoe_invoke.h (extern) and we provide it here.
 * Eventually we'll move this to a libqsoe init module so every
 * standalone consumer shares the same definition. */
seL4_IPCBuffer *qsoe_ipcbuf;

/* Spawn convention from spawn.c: the kernel maps the IPC buffer at
 * this virtual address in the child's VSpace. */
#define TESTER_IPC_BUFFER ((seL4_IPCBuffer *)0x1FE000UL)

static void putu(unsigned long x)
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

int main(pid_t pid)
{
    qsoe_ipcbuf = TESTER_IPC_BUFFER;

    sel4_debug_puts("[tester] alive, pid=");
    {
        char d = '0' + (pid & 0x7);
        sel4_debug_putchar(d);
        sel4_debug_putchar('\n');
    }

    /* taskman's primary channel is chid 1 (registered by hand at
     * taskman boot, see Design ch. 3 §3.2 — "the primary endpoint"). */
    int coid = ConnectAttach(ND_LOCAL_NODE, QSOE_PID_TASKMAN, 1, 0, 0);
    if (coid < 0) {
        sel4_debug_puts("[tester] ConnectAttach FAILED errno=");
        putu((unsigned long)qsoe_errno);
        sel4_debug_putchar('\n');
        for (;;) __asm__ volatile("nop");
    }
    sel4_debug_puts("[tester] ConnectAttach -> coid=");
    sel4_debug_putchar('0' + coid);
    sel4_debug_putchar('\n');

    /* Three round-trips. Each sends a counter; taskman's dispatch echoes
     * it back +1 so we see the protocol working. */
    for (int i = 1; i <= 3; ++i) {
        unsigned long payload = (unsigned long)i;
        unsigned long reply   = 0;
        int rc = MsgSend(coid, &payload, sizeof payload,
                          &reply, sizeof reply);
        if (rc < 0) {
            sel4_debug_puts("[tester] MsgSend FAILED\n");
            break;
        }
        sel4_debug_puts("[tester] MsgSend(");
        sel4_debug_putchar('0' + i);
        sel4_debug_puts(") -> ");
        sel4_debug_putchar('0' + (char)(reply & 0xF));
        sel4_debug_putchar('\n');
    }

    int dr = ConnectDetach(coid);
    sel4_debug_puts("[tester] ConnectDetach -> ");
    sel4_debug_putchar(dr == 0 ? '0' : 'F');
    sel4_debug_putchar('\n');

    sel4_debug_puts("[tester] done\n");
    for (;;) __asm__ volatile("nop");
    return 0;
}
