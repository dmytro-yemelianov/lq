/*
 * tester — v0.3.0 stub.
 *
 * Currently just streams a recognisable character (T) to the kernel
 * console via seL4_DebugPutChar so we can see it's running on its own
 * TCB. v0.3.2 replaces the body with real libqsoe IPC round-trips.
 */

#include "../taskman/sel4_syscalls.h"

typedef int pid_t;

int main(pid_t pid)
{
    sel4_debug_puts("[tester] alive, pid=");
    {
        char d = '0' + (pid & 0x7);
        sel4_debug_putchar(d);
        sel4_debug_putchar('\n');
    }
    /* Stream Ts as a heartbeat. v0.3.2 replaces this with the actual
     * IPC round-trips against taskman. */
    for (;;) {
        for (volatile unsigned long i = 0; i < 5000000; ++i) { }
        sel4_debug_putchar('T');
    }
    return 0;
}
