/*
 * init — QSOE's userland orchestrator (pid 2 by convention).
 *
 * v0.6.1: minimal /sbin/init. taskman drops us at the head of every
 * boot; we're responsible for spawning the rest of userland.
 *
 * For v0.6.1 step 2 we do the simplest possible thing: spawn tester
 * (which carries the regression test suite) and waitpid for it to
 * exit. v0.6.1's later steps add spawning devc-ser8250 and switching
 * /dev/console to it before tester runs.
 *
 * Long-term this turns into a real /sbin/init: read a config from
 * the cpio, spawn the listed drivers, perform their detach
 * synchronisation, eventually run getty on the console. v0.7+ work.
 */

#include <stdio.h>
#include "../taskman/sel4_syscalls.h"
#include "../taskman/qsoe_invoke.h"
#include "../libqsoe/include/qsoe/qrv.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("[init] alive, pid=%d\n", (int)qsoe_self_pid);
    fflush(stdout);

    /* (1) Spawn the UART driver. devc-ser8250 will set up the
     *     16550, bind its IRQ Notification, register itself at
     *     /dev/ser1, then call procmgr_detach(0) to unblock us. */
    pid_t drv_pid = 0;
    char *drv_argv[] = { "devc-ser8250", 0 };
    int rc = posix_spawn(&drv_pid, "devc-ser8250.elf", 0, 0, drv_argv, 0);
    if (rc != 0) {
        printf("[init] posix_spawn(devc-ser8250) failed, rc=%d\n", rc);
        fflush(stdout);
        return 1;
    }
    printf("[init] spawned devc-ser8250, pid=%d\n", (int)drv_pid);
    fflush(stdout);

    int dstatus = 0;
    int wrc = waitpid(drv_pid, &dstatus, 0);
    if (wrc < 0 || dstatus != 0) {
        printf("[init] devc-ser8250 detach failed (status=%d)\n", dstatus);
        fflush(stdout);
        return 1;
    }
    printf("[init] devc-ser8250 ready (detached with status %d)\n", dstatus);
    fflush(stdout);

    /* (2) Swap /dev/console to point at the new driver. Existing fds
     *     in this process keep working (their caps are on the old
     *     in-taskman console channel); only NEW opens (e.g. tester's
     *     stdio inheritance at spawn time) see the redirect. */
    int prc = qsoe_pathmgr_repath("/dev/console",
                                   drv_pid, /*chid=*/1,
                                   /*handler_kind=external*/0);
    if (prc != 0) {
        printf("[init] pathmgr_repath(/dev/console) failed, errno=%d\n",
               qsoe_errno);
        fflush(stdout);
    } else {
        printf("[init] /dev/console now -> (%d, 1) [devc-ser8250]\n",
               (int)drv_pid);
        fflush(stdout);
    }

    /* (3) Now spawn tester. Its inherited fds 0/1/2 are minted by
     *     spawn.c against /dev/console's current entry — which is
     *     devc-ser8250. tester's printf bytes will flow through the
     *     real driver. */
    char *t_argv[] = { "hello", "world", 0 };
    char *t_envp[] = { "FOO=bar", "QSOE_VER=0.4.4", 0 };
    pid_t tester_pid = 0;
    rc = posix_spawn(&tester_pid, "tester.elf", 0, 0, t_argv, t_envp);
    if (rc != 0) {
        printf("[init] posix_spawn(tester) failed, rc=%d\n", rc);
        fflush(stdout);
        return 1;
    }
    printf("[init] spawned tester, pid=%d\n", (int)tester_pid);
    fflush(stdout);

    int status = 0;
    wrc = waitpid(tester_pid, &status, 0);
    if (wrc < 0) {
        printf("[init] waitpid(tester) failed\n");
        fflush(stdout);
        return 1;
    }
    printf("[init] tester exited, status=%d\n", status);
    fflush(stdout);

    for (;;) qsoe_sys_yield();
}
