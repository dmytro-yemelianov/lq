/*
 * hello — the first non-taskman/non-tester QSOE userland program.
 *
 * v0.4.1: spawned by tester via posix_spawn("hello.elf"). Prints its
 * pid, returns 42 — which crt0 turns into _exit(42), driving the
 * ProcessTerminate wire path back to taskman.
 *
 * v0.4.3: acts as a tiny server. ChannelCreate gets chid=1; loops
 * MsgReceive + MsgReply three times, echoing payload + 1 — same
 * shape taskman's dispatcher uses. Proves QSOE supports non-taskman
 * servers end-to-end (ConnectAttach from another process, MsgSend
 * over a real IPC pipe, ConnectServerInfo introspection).
 *
 * Future v0.5+: this is the seed of a real /sbin/init or /bin/hello
 * shipped in the userland CPIO.
 */

#include "../taskman/sel4_syscalls.h"
#include "../taskman/sel4_types.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"

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

int main(int argc, char **argv, char **envp)
{
    sel4_debug_puts("[hello] alive, pid=");
    putd((int)qsoe_self_pid);
    sel4_debug_puts(" argc=");
    putd(argc);
    for (int i = 0; i < argc; ++i) {
        sel4_debug_puts(" argv[");
        putd(i);
        sel4_debug_puts("]=");
        sel4_debug_puts(argv[i]);
    }
    for (int i = 0; envp && envp[i]; ++i) {
        sel4_debug_puts(" envp[");
        putd(i);
        sel4_debug_puts("]=");
        sel4_debug_puts(envp[i]);
    }
    sel4_debug_putchar('\n');

    int chid = ChannelCreate(0);
    sel4_debug_puts("[hello] ChannelCreate -> chid=");
    putd(chid);
    sel4_debug_putchar('\n');

    for (int i = 0; i < 3; ++i) {
        unsigned long payload = 0;
        struct _msg_info mi;
        int rcvid = MsgReceive(chid, &payload, sizeof payload, &mi);
        sel4_debug_puts("[hello] MsgReceive rcvid=");
        putd(rcvid);
        sel4_debug_puts(" pid=");
        putd((int)mi.pid);
        sel4_debug_puts(" payload=");
        putd((int)payload);
        sel4_debug_putchar('\n');

        unsigned long reply = payload + 1;
        MsgReply(rcvid, 0, &reply, sizeof reply);
    }

    ChannelDestroy(chid);
    sel4_debug_puts("[hello] exiting with status 42\n");
    return 42;
}
