/*
 * hello — the first non-taskman/non-tester QSOE userland program.
 *
 * v0.4.1: spawned by tester via posix_spawn("hello.elf"). Prints its
 * pid, returns 42 — which crt0 turns into _exit(42), driving the
 * ProcessTerminate wire path back to taskman.
 *
 * Future v0.5+: this is the seed of a real /sbin/init or /bin/hello
 * shipped in the userland CPIO.
 */

#include "../taskman/sel4_syscalls.h"
#include "../taskman/sel4_types.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"

/* Same spawn-convention IPC buffer vaddr as tester. */
#define HELLO_IPC_BUFFER ((seL4_IPCBuffer *)0x1FE000UL)

int main(pid_t pid)
{
    qsoe_libqsoe_init(HELLO_IPC_BUFFER, pid);
    sel4_debug_puts("[hello] alive, pid=");
    char d = '0' + (char)(pid & 0x7);
    sel4_debug_putchar(d);
    sel4_debug_putchar('\n');
    sel4_debug_puts("[hello] exiting with status 42\n");
    return 42;
}
