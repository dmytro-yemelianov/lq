/*
 * taskman — central system server (QSOE).
 *
 * v0.2: wires up the four QNX-style IPC lifecycle calls
 * (ChannelCreate / ChannelDestroy / ConnectAttach / ConnectDetach)
 * via libqsoe and exercises them in a self-test. The dispatch loop
 * for serving these calls from *other* processes lands in v0.3, when
 * we have a second process to call from.
 */

#include "sel4_syscalls.h"
#include "sel4_types.h"
#include "qsoe_invoke.h"
#include "server.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"
#include <qsoe/sys_version.h>

#define QSOE_STR_(x) #x
#define QSOE_STR(x)  QSOE_STR_(x)
#define QSOE_VSHORT  "v" QSOE_STR(QSOE_VERSION_MAJOR) "." QSOE_STR(QSOE_VERSION_MINOR)

/* Single-threaded taskman in v0.x: a plain global suffices. */
seL4_IPCBuffer *qsoe_ipcbuf;

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

static void report(const char *op, int result)
{
    sel4_debug_puts("  ");
    sel4_debug_puts(op);
    sel4_debug_puts(" -> ");
    if (result < 0) {
        sel4_debug_puts("FAIL errno=");
        putu((unsigned long)qsoe_errno);
    } else {
        sel4_debug_puts("OK (");
        putu((unsigned long)result);
        sel4_debug_putchar(')');
    }
    sel4_debug_putchar('\n');
}

static unsigned cstrlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

static void print_banner(void)
{
    /* Clear-screen via Ctrl-L, then a bordered banner sized to fit
     * "QSOE: Quick & Secure Operating Environment <version>". */
    static const char *prefix = "QSOE: Quick & Secure Operating Environment ";
    unsigned inner = 1 + cstrlen(prefix) + cstrlen(QSOE_VSHORT) + 1;

    sel4_debug_putchar('\f');

    sel4_debug_putchar('+');
    for (unsigned i = 0; i < inner; ++i) sel4_debug_putchar('-');
    sel4_debug_puts("+\n");

    sel4_debug_puts("| ");
    sel4_debug_puts(prefix);
    sel4_debug_puts(QSOE_VSHORT);
    sel4_debug_puts(" |\n");

    sel4_debug_putchar('+');
    for (unsigned i = 0; i < inner; ++i) sel4_debug_putchar('-');
    sel4_debug_puts("+\n\n");
}

static seL4_CPtr find_largest_ram_untyped(seL4_BootInfo *bi)
{
    unsigned n = bi->untyped.end - bi->untyped.start;
    unsigned best = (unsigned)-1;
    unsigned best_bits = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (bi->untypedList[i].isDevice) continue;
        if (bi->untypedList[i].sizeBits > best_bits) {
            best_bits = bi->untypedList[i].sizeBits;
            best = i;
        }
    }
    if (best == (unsigned)-1) return 0;
    return bi->untyped.start + best;
}

int main(seL4_BootInfo *bi)
{
    print_banner();
    qsoe_invoke_init(bi->ipcBuffer);

    seL4_CPtr ut = find_largest_ram_untyped(bi);
    if (ut == 0) {
        sel4_debug_puts("FATAL: no RAM untyped\n");
        for (;;) __asm__ volatile("nop");
    }

    tm_init(ut, seL4_CapInitThreadCNode, bi->empty.start);

    /* Test 1: single channel, single connection, full lifecycle. */
    sel4_debug_puts("test 1: single channel lifecycle\n");
    int chid = ChannelCreate(0);
    report("ChannelCreate", chid);
    int coid = ConnectAttach(ND_LOCAL_NODE, QSOE_PID_TASKMAN, chid, 0, 0);
    report("ConnectAttach", coid);
    int r1 = ConnectDetach(coid);
    report("ConnectDetach", r1);
    int r2 = ChannelDestroy(chid);
    report("ChannelDestroy", r2);
    int t1_ok = (chid > 0 && coid > 0 && r1 == 0 && r2 == 0);

    /* Test 2: two channels coexist; destroy the correct one. */
    sel4_debug_puts("test 2: two channels, targeted destroy\n");
    int a = ChannelCreate(0);
    int b = ChannelCreate(0);
    report("ChannelCreate(a)", a);
    report("ChannelCreate(b)", b);
    int rb = ChannelDestroy(b);
    report("ChannelDestroy(b)", rb);
    int ra = ChannelDestroy(a);
    report("ChannelDestroy(a)", ra);
    int t2_ok = (a > 0 && b > 0 && a != b && ra == 0 && rb == 0);

    /* Test 3: small loop — verify we can churn without leaking handles
     * (slots leak by design in v0.2 since the bump allocator doesn't
     * recycle, but the libqsoe chid/coid tables must recycle). */
    sel4_debug_puts("test 3: 8-iteration churn\n");
    int t3_ok = 1;
    for (int i = 0; i < 8; ++i) {
        int c  = ChannelCreate(0);
        int co = ConnectAttach(ND_LOCAL_NODE, QSOE_PID_TASKMAN, c, 0, 0);
        int rd = ConnectDetach(co);
        int rc = ChannelDestroy(c);
        if (!(c > 0 && co > 0 && rd == 0 && rc == 0)) t3_ok = 0;
    }
    sel4_debug_puts(t3_ok ? "  loop OK\n" : "  loop FAILED\n");

    if (t1_ok && t2_ok && t3_ok) {
        sel4_debug_puts("v0.2 self-test PASSED\n");
    } else {
        sel4_debug_puts("v0.2 self-test FAILED\n");
    }

    for (;;) __asm__ volatile("nop");
    return 0;
}
