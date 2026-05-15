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

/* v0.6.4 signal-self-test.  Handler runs in the signal thread; sets
 * a flag the main thread polls below.  If we get back to main with
 * got_sigusr1 = 1, the whole pulse-as-signal path worked end-to-end:
 *   raise(SIGUSR1)
 *     → ConnectAttach(self, signal_chid)
 *     → MsgSendPulse(coid, code=SIGUSR1)
 *     → signal thread wakes from MsgReceive
 *     → looks up qsoe_signal_handlers[SIGUSR1]
 *     → invokes us here. */
#define SIGUSR1 10
typedef void (*sighandler_t)(int);
extern sighandler_t signal(int sig, sighandler_t fn);
extern int          raise(int sig);

static volatile int got_sigusr1;

static void sigusr1_handler(int sig)
{
    (void)sig;
    got_sigusr1 = 1;
}

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

    /* v0.6.4 — signal self-test.  Install a handler, raise() the
     * signal to self, wait for the signal thread to run it. */
    extern int qsoe_signal_chid;
    extern int qsoe_signal_init_chid_err;
    extern int qsoe_signal_init_thread_err;
    extern int qsoe_signal_init_done;
    extern int qsoe_signal_init_entered;
    extern int qsoe_signal_init_past_chid_check;
    extern int qsoe_signal_init_chid_seen;
    printf("[hello] signal-test: entered=%d past=%d chid_seen=%d "
           "chid=%d done=%d chid_err=%d thread_err=%d\n",
           qsoe_signal_init_entered, qsoe_signal_init_past_chid_check,
           qsoe_signal_init_chid_seen,
           qsoe_signal_chid, qsoe_signal_init_done,
           qsoe_signal_init_chid_err, qsoe_signal_init_thread_err);
    fflush(stdout);
    signal(SIGUSR1, sigusr1_handler);
    printf("[hello] signal-test: handler installed, calling raise(SIGUSR1)\n");
    fflush(stdout);
    int raise_rc = raise(SIGUSR1);
    printf("[hello] signal-test: raise(SIGUSR1) -> %d\n", raise_rc);
    fflush(stdout);

    /* Crude wait for the signal thread to schedule. */
    for (int i = 0; i < 10000000 && !got_sigusr1; ++i)
        for (volatile int j = 0; j < 100; ++j);
    if (got_sigusr1)
        printf("[hello] signal-test: PASS — handler ran in signal thread\n");
    else
        printf("[hello] signal-test: FAIL — handler did not run "
               "(got_sigusr1=%d)\n", got_sigusr1);
    fflush(stdout);

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
