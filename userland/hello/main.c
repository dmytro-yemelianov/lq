/*
 * hello — the first non-taskman/non-tester QSOE userland program.
 *
 * v0.5.1: switched from raw sel4_debug_puts to musl libc printf,
 * proving the end-to-end stack:
 *
 *   printf("hello, %s\n", argv[1])
 *     -> musl vfprintf
 *     -> __stdio_write(stdout, ...)
 *     -> syscall(SYS_writev, 1, iov, 1)        (musl, indirect via __sysinfo)
 *     -> qsoe_syscall_dispatch                  (libqsoe/syscall_dispatch.c)
 *     -> qsoe_writev -> qsoe_write              (libqsoe/io.c)
 *     -> MsgSend(TM_REQ_IO_WRITE) to taskman
 *     -> taskman /dev/console handler
 *     -> sel4_debug_putchar per byte
 *
 * v0.4.3 server demo (ChannelCreate + MsgReceive loop) lives on in
 * the tail of main() so multi-server IPC still gets exercised.
 */

#include <stdio.h>
#include "../taskman/sel4_syscalls.h"
#include "../taskman/sel4_types.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"

int main(int argc, char **argv, char **envp)
{
    /* The printf demo. If you're reading this output, every layer
     * below — musl, __sysinfo bridge, libqsoe IO, taskman pathmgr,
     * console resmgr — is working. */
    printf("[hello] printf says hi from pid=%d\n", (int)qsoe_self_pid);
    printf("[hello] argc=%d\n", argc);
    for (int i = 0; i < argc; ++i) {
        printf("[hello]   argv[%d]=%s\n", i, argv[i]);
    }
    for (int i = 0; envp && envp[i]; ++i) {
        printf("[hello]   envp[%d]=%s\n", i, envp[i]);
    }
    fprintf(stderr, "[hello] this line went to stderr (fd=2)\n");
    fflush(stdout);
    fflush(stderr);

    /* v0.4.3 server demo — ChannelCreate + MsgReceive loop. */
    int chid = ChannelCreate(0);
    printf("[hello] ChannelCreate -> chid=%d\n", chid);
    fflush(stdout);

    for (int i = 0; i < 3; ++i) {
        unsigned long payload = 0;
        struct _msg_info mi;
        int rcvid = MsgReceive(chid, &payload, sizeof payload, &mi);
        printf("[hello] MsgReceive rcvid=%d payload=%lu\n", rcvid, payload);
        fflush(stdout);

        unsigned long reply = payload + 1;
        MsgReply(rcvid, 0, &reply, sizeof reply);
    }

    ChannelDestroy(chid);
    printf("[hello] exiting with status 42\n");
    fflush(stdout);
    return 42;
}
