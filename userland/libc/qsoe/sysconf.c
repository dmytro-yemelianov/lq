/*
 * sysconf.c — POSIX sysconf().
 *
 * Returns system-configuration values for QSOE.  Most are compile-
 * time constants derived from the kernel page size, libqsoe's fd
 * pool size, and other invariants; the ones that don't apply
 * (job control, real-time options, …) return -1 with errno
 * untouched per POSIX.
 *
 * No IPC needed — every answer comes from a constant.  If a
 * caller asks about something we don't know, return -1 + EINVAL.
 */

#include <unistd.h>
#include <qsoe/qrv.h>

long sysconf(int name)
{
    switch (name) {
    case _SC_PAGESIZE:          /* a.k.a. _SC_PAGE_SIZE */
        return 4096L;           /* RISC-V Sv39 base page */
    case _SC_CLK_TCK:
        return 100L;            /* QSOE schedules at 100 Hz today */
    case _SC_OPEN_MAX:
        return 256L;            /* QSOE_MAX_FD_CONNECTIONS in libqsoe */
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        return 4L;              /* qemu-riscv-virt SMP boot */
    case _SC_PHYS_PAGES:
        return 32768L;          /* 128 MiB / 4 KiB — placeholder pending
                                 * a TM_REQ_SYSINFO when taskman tracks
                                 * physical-memory accounting */
    case _SC_ARG_MAX:
        return 4096L;           /* taskman's s_staging in main.c */
    case _SC_CHILD_MAX:
        return 7L;              /* TM_MAX_PROCESSES - 1 (pid 1 = taskman) */
    case _SC_LINE_MAX:
        return 2048L;
    case _SC_LOGIN_NAME_MAX:
        return 32L;
    case _SC_HOST_NAME_MAX:
        return 64L;
    case _SC_TTY_NAME_MAX:
        return 32L;
    case _SC_VERSION:
        return 200809L;         /* POSIX.1-2008 */
    default:
        qsoe_errno = EINVAL;
        return -1L;
    }
}
