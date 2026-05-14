/*
 * <qsoe/slots.h> — well-known CSpace slot conventions, shared by libqsoe
 * and taskman. See Design doc §4.5 "CSpace conventions".
 */
#ifndef QSOE_SLOTS_H
#define QSOE_SLOTS_H

#define QSOE_CAP_NULL          0
#define QSOE_CAP_TASKMAN_EP    1   /* Send cap to taskman's primary endpoint */
#define QSOE_CAP_OWN_UNTYPED   2   /* This process's untyped budget (v0.4.1+) */

/* v0.5.0: spawn-time stdio inheritance. taskman mints three badged
 * Send-caps to (taskman, CONSOLE_CHID) into these slots; libqsoe's
 * init binds them to fds 0/1/2 so musl's stdin/stdout/stderr work
 * before main() runs. The slots are stable across the program's
 * lifetime — close() on fds 0/1/2 deletes the cap but leaves the
 * slot for re-allocation by the libqsoe slot bookkeeping. */
#define QSOE_CAP_STDIN_CONNECT  3
#define QSOE_CAP_STDOUT_CONNECT 4
#define QSOE_CAP_STDERR_CONNECT 5

/* v0.6.1 driver-special-case slots. taskman mints these into devc-*
 * children at spawn (gated on the ELF name for now; manifest-driven
 * cap granting comes later). Stay within QSOE_CAP_WELL_KNOWN_END. */
#define QSOE_CAP_IRQ_HANDLER    6  /* IRQHandler for the driver's IRQ line */
#define QSOE_CAP_UART_FRAME     7  /* 4 KiB device-untyped covering UART MMIO */
#define QSOE_CAP_IRQ_NTFN       8  /* Notification the IRQHandler is bound to */

#define QSOE_CAP_WELL_KNOWN_END 16 /* slots [2..15] reserved; dynamics start at 16 */

/* By convention pid 1 is taskman itself. */
#define QSOE_PID_TASKMAN       1

#endif /* QSOE_SLOTS_H */
