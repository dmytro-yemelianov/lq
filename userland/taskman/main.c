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
#include "spawn.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"
#include "../libqsoe/include/qsoe/wire.h"
#include <qsoe/sys_version.h>
#include <cpio/cpio.h>

extern char _userland_cpio_start[];
extern char _userland_cpio_end[];

/* Dispatch one incoming message. Inputs: the request's badge + MRs.
 * Outputs: the reply tag and the reply MRs (the first 1-2 of which
 * may carry returned slots, etc.). */
static seL4_MessageInfo_t
tm_dispatch(seL4_MessageInfo_t info, seL4_Word badge,
            seL4_Word mr0, seL4_Word mr1, seL4_Word mr2, seL4_Word mr3,
            seL4_Word *out_mr0, seL4_Word *out_mr1)
{
    (void)mr3;
    pid_t caller = (pid_t)badge;
    unsigned label = (unsigned)seL4_MessageInfo_get_label(info);
    seL4_Word err = 0;
    seL4_Word reply_len = 0;
    *out_mr0 = 0;
    *out_mr1 = 0;

    switch (label) {
    case TM_REQ_CHANNEL_CREATE: {
        int chid = (int)mr0;
        unsigned flags = (unsigned)mr1;
        unsigned long recv_slot = 0;
        int rc = tm_channel_create(caller, chid, flags, &recv_slot);
        if (rc) { err = (seL4_Word)(-rc); }
        else    { *out_mr0 = recv_slot; reply_len = 1; }
        break;
    }
    case TM_REQ_CHANNEL_DESTROY: {
        seL4_CPtr recv_slot = (seL4_CPtr)mr0;
        int rc = tm_channel_destroy(caller, recv_slot);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_CONNECT_ATTACH: {
        pid_t target_pid = (pid_t)mr0;
        int target_chid = (int)mr1;
        unsigned flags = (unsigned)mr2;
        unsigned long send_slot = 0;
        int rc = tm_connect_attach(caller, target_pid, target_chid,
                                    flags, &send_slot);
        if (rc) { err = (seL4_Word)(-rc); }
        else    { *out_mr0 = send_slot; reply_len = 1; }
        break;
    }
    case TM_REQ_CONNECT_DETACH: {
        seL4_CPtr send_slot = (seL4_CPtr)mr0;
        int rc = tm_connect_detach(caller, send_slot);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    default:
        /* Application-level message (v0.3 demo). Echo back with MR0
         * incremented, so tester sees the round-trip succeed. */
        *out_mr0 = mr0 + 1;
        reply_len = 1;
        break;
    }
    return seL4_MessageInfo_new(err, 0, 0, reply_len);
}

#define QSOE_STR_(x) #x
#define QSOE_STR(x)  QSOE_STR_(x)
#define QSOE_VSHORT  "v" QSOE_STR(QSOE_VERSION_MAJOR) "." QSOE_STR(QSOE_VERSION_MINOR)

/* Single-threaded taskman in v0.x: a plain global suffices. */
seL4_IPCBuffer *qsoe_ipcbuf;

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

    /* v0.3.0: spawn tester. We don't yet have taskman's primary
     * endpoint allocated by ChannelCreate — for v0.3.0 we create it
     * the same way we will in v0.3.2, by hand, so that spawn() can
     * mint a Send cap to it for tester's CSpace slot 1. */
    seL4_CPtr primary_ep = bi->empty.start; /* very next free slot */
    /* Bump tm_init's allocator past it so spawn() doesn't reuse it. */
    extern seL4_CPtr s_next_slot;
    s_next_slot = bi->empty.start + 1;
    seL4_Word rerr = qsoe_untyped_retype(ut, seL4_EndpointObject, 0,
                                          seL4_CapInitThreadCNode, 0, 0,
                                          primary_ep, 1);
    if (rerr != 0) {
        sel4_debug_puts("FATAL: failed to retype primary endpoint\n");
        for (;;) __asm__ volatile("nop");
    }
    /* Register taskman's primary channel in the registry so ConnectAttach
     * from other processes can find it as (pid=1, chid=1). The master
     * and recv cap are the same slot — taskman invokes the cap directly
     * for both seL4_Recv (server-side) and as the mint source for new
     * connections. */
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, 1,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register primary channel\n");
        for (;;) __asm__ volatile("nop");
    }

    /* Find tester.elf in the embedded userland CPIO. */
    unsigned long cpio_len = (unsigned long)
        (_userland_cpio_end - _userland_cpio_start);
    unsigned long elf_size = 0;
    const void *elf = cpio_get_file(_userland_cpio_start, cpio_len,
                                     "tester.elf", &elf_size);
    if (!elf) {
        sel4_debug_puts("FATAL: tester.elf not found in CPIO\n");
        for (;;) __asm__ volatile("nop");
    }
    sel4_debug_puts("taskman: spawning tester (pid 2)...\n");
    int sr = tm_spawn(elf, elf_size, 2, primary_ep);
    if (sr != 0) {
        sel4_debug_puts("FATAL: tm_spawn returned non-zero\n");
        for (;;) __asm__ volatile("nop");
    }
    sel4_debug_puts("taskman: dispatcher ready\n");

    /* Dispatch loop: ReplyRecv pattern. seL4_Recv blocks until the
     * first message; thereafter ReplyRecv atomically sends the reply
     * to the previous caller and waits for the next. */
    seL4_Word badge;
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t info = qsoe_sys_recv(primary_ep, &badge,
                                             &mr0, &mr1, &mr2, &mr3);
    for (;;) {
        seL4_Word reply_mr0, reply_mr1;
        seL4_MessageInfo_t reply_info = tm_dispatch(info, badge,
                                                     mr0, mr1, mr2, mr3,
                                                     &reply_mr0, &reply_mr1);
        mr0 = reply_mr0;
        mr1 = reply_mr1;
        mr2 = 0; mr3 = 0;
        info = qsoe_sys_reply_recv(primary_ep, reply_info, &badge,
                                    &mr0, &mr1, &mr2, &mr3);
    }
    return 0;
}
