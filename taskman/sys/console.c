/*
 * console.c — /dev/console resource manager (v0.5+).
 *
 * The console resmgr is hosted inside taskman: write traffic walks
 * msg[4..] one byte at a time, invoking the kernel-debug SBI
 * putchar. Reads are stubbed to EAGAIN until a UART driver lands
 * (the seL4 kernel doesn't expose SBI getchar through a debug
 * syscall, and we have no 16550 driver yet).
 *
 * Dispatch wires up these handlers via the (badge -> connection ->
 * channel) lookup chain in main.c — any TM_REQ_IO_WRITE / IO_READ
 * whose connection resolves to TM_CONSOLE_CHID lands here.
 */

#include "console.h"
#include "../sel4_types.h"
#include "../tm_log.h"
#include <sys/qsoe.h>
#include <qsoe/ipcbuf.h>

/* Bound by the IPC buffer payload area (msg[4..119] = 116 words =
 * 928 bytes). Larger writes are chunked client-side in qsoe_write. */
#define TM_CONSOLE_MAX_WRITE  928

/* Write nbytes from msg[4..] to the console. Returns number of bytes
 * actually written (= nbytes on success). The caller is expected to
 * have validated nbytes against TM_CONSOLE_MAX_WRITE; we clamp
 * defensively. */
unsigned tm_console_write(unsigned nbytes)
{
    if (nbytes > TM_CONSOLE_MAX_WRITE) nbytes = TM_CONSOLE_MAX_WRITE;
    const unsigned char *bytes = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < nbytes; ++i) {
        tm_raw_putc((char)bytes[i]);
    }
    return nbytes;
}

/* v0.5.0 read stub: no input available. Caller fills the reply with
 * label=EAGAIN, MR0=0. */
int tm_console_read(unsigned want, unsigned *out_got)
{
    (void)want;
    *out_got = 0;
    return -EAGAIN;
}

int tm_console_stat(tm_stat_t *out)
{
    if (!out) return -EINVAL;
    unsigned char *p = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof *out; ++i) p[i] = 0;

    out->st_dev     = 5;                  /* synthetic dev for /dev/console */
    out->st_ino     = 1;
    out->st_mode    = TM_S_IFCHR | 0666;  /* character device, rw for all */
    out->st_nlink   = 1;
    out->st_uid     = 0;
    out->st_gid     = 0;
    out->st_rdev    = (5UL << 8) | 1;     /* maj=5, min=1 */
    out->st_size    = 0;
    out->st_blksize = 256;
    return 0;
}
