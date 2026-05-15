/*
 * syscall_dispatch.c — bridge from musl's __sysinfo function pointer
 * to libqsoe's POSIX wrappers (v0.5.1+).
 *
 * The QSOE-patched musl in core/userland/libc/patches/arch/riscv64/
 * routes all "syscalls" through:
 *     ((long(*)(long, ...))__sysinfo)(n, ...)
 * instead of an ecall. _qsoe_start_main plants the address of
 * qsoe_syscall_dispatch into __sysinfo before main() runs, so every
 * musl libc call lands here in pure C and is routed to a libqsoe
 * implementation (which then does the seL4 IPC).
 *
 * SYS_* numbers are the Linux RISC-V values musl generates from
 * arch/riscv64/bits/syscall.h.in. We don't include that header (it
 * would drag in musl's full toolchain) — instead we hardcode the
 * handful we handle.
 */

#include "../include/qsoe/qrv.h"
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#define SYS_close       57
#define SYS_openat      56
#define SYS_lseek       62
#define SYS_read        63
#define SYS_write       64
#define SYS_readv       65
#define SYS_writev      66
#define SYS_exit        93
#define SYS_exit_group  94
#define SYS_mmap       222
#define SYS_set_tid_address 96
#define SYS_ioctl       29
#define SYS_set_robust_list 99
#define SYS_clock_gettime 113

/* Minimal iovec view of struct iovec from musl. The musl definition
 * lives in <sys/uio.h> via bits/alltypes.h; ours matches in layout. */
struct musl_iovec {
    void         *iov_base;
    unsigned long iov_len;
};

/* Forward decl of qsoe_mmap (real impl below).  Memory comes from
 * taskman's Memory Manager via TM_REQ_MMAP — no brk anywhere in
 * QSOE. */
void *qsoe_mmap(void *addr, unsigned long length, int prot, int flags,
                int fd, long off);

static long do_writev(int fd, const struct musl_iovec *iov, int iovcnt)
{
    if (iovcnt < 0) return -EINVAL;
    long total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (iov[i].iov_len == 0) continue;
        long rc = qsoe_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (rc < 0) return total ? total : -1;
        total += rc;
        if ((unsigned long)rc < iov[i].iov_len) break;
    }
    return total;
}

/* musl uses openat with AT_FDCWD = -100 for plain open(). We ignore
 * dirfd for v0.5.1 (no current-working-directory concept yet). */
static int do_openat(int dirfd, const char *path, int flags)
{
    (void)dirfd;
    return qsoe_open(path, flags);
}

/* Linux returns -errno on syscall failure, but its convention is to
 * use values in [-4095, -1] as errors. musl's syscall macros decode
 * by checking `unsigned long >= -4095`. We follow the same pattern:
 * negative-errno on failure, non-negative result on success. The
 * libqsoe wrappers return -1 + set qsoe_errno, so we re-encode. */
static long ret(long rc)
{
    if (rc >= 0) return rc;
    return -qsoe_errno;
}

long qsoe_syscall_dispatch(long n, long a, long b, long c, long d, long e, long f);
long qsoe_syscall_dispatch(long n, long a, long b, long c, long d, long e, long f)
{
    (void)d; (void)e; (void)f;
    switch (n) {
    case SYS_write:
        return ret(qsoe_write((int)a, (const void *)b, (unsigned long)c));
    case SYS_writev:
        return do_writev((int)a, (const struct musl_iovec *)b, (int)c);
    case SYS_read:
        return ret(qsoe_read((int)a, (void *)b, (unsigned long)c));
    case SYS_readv: {
        /* readv: walk iovecs, calling qsoe_read for each. v0.5.1
         * console read stubs to EAGAIN, so this returns 0/-EAGAIN. */
        const struct musl_iovec *iov = (const struct musl_iovec *)b;
        int iovcnt = (int)c;
        long total = 0;
        for (int i = 0; i < iovcnt; ++i) {
            if (iov[i].iov_len == 0) continue;
            long rc = qsoe_read((int)a, iov[i].iov_base, iov[i].iov_len);
            if (rc < 0) return total ? total : -qsoe_errno;
            total += rc;
            if ((unsigned long)rc < iov[i].iov_len) break;
        }
        return total;
    }
    case SYS_openat:
        return ret(do_openat((int)a, (const char *)b, (int)c));
    case SYS_close:
        return ret(qsoe_close((int)a));
    case SYS_lseek:
        /* v0.5.1: not supported for the console; printf doesn't seek. */
        return -ESPIPE;
    case SYS_mmap:
        /* musl's __mmap calls this: a=addr, b=length, c=prot, d=flags,
         * e=fd, f=offset.  qsoe_mmap routes to taskman's Memory
         * Manager via TM_REQ_MMAP. */
        return (long)qsoe_mmap((void *)a, (unsigned long)b, (int)c,
                               (int)d, (int)e, (long)f);
    case SYS_exit:
    case SYS_exit_group:
        _exit((int)a);
        /* not reached */
        return 0;
    case SYS_set_tid_address:
        /* musl uses this during init to learn its tid. Single-thread
         * processes just get a 1; libqsoe's qsoe_self_pid would be
         * the QNX-correct answer once we have full pthread support. */
        return 1;
    case SYS_set_robust_list:
        return 0;  /* harmless no-op */
    case SYS_ioctl:
        /* musl's stdio probes the fd with TIOCGWINSZ etc. — return
         * ENOTTY so it treats stdout as a regular pipe (no
         * line-buffering on terminals). */
        return -ENOTTY;
    case SYS_clock_gettime:
        /* musl uses CLOCK_MONOTONIC for various timeouts. Fake-zero
         * is OK for printf paths. */
        if (b) {
            unsigned long *ts = (unsigned long *)b;
            ts[0] = 0;
            ts[1] = 0;
        }
        return 0;
    default:
        return -ENOSYS;
    }
}

/* musl normally puts errno in a per-thread struct reached via tp.
 * We don't run __libc_start_main, so musl's pthread bootstrap never
 * happens; provide our own __errno_location that returns a single
 * static int. This overrides the weak symbol from musl's
 * src/errno/__errno_location.c by the usual archive-vs-object linker
 * precedence (libqsoe is on the link line before libc.a). */
static int qsoe_libc_errno;

int *__errno_location(void);
int *__errno_location(void) { return &qsoe_libc_errno; }

/* Some musl source paths reference ___errno_location (extra
 * underscore) as a weak alias. Provide the same definition under
 * that name so the linker can resolve either. */
int *___errno_location(void) __attribute__((weak, alias("__errno_location")));

/* qsoe_mmap — POSIX mmap routed to taskman's Memory Manager.
 *
 * v0.6.4 supports MAP_ANONYMOUS only.  addr/prot/flags/fd/off are
 * ignored except for sanity: an mmap of a file (fd >= 0) is rejected
 * with -ENODEV.  Taskman rounds length up to 2 MiB and returns a
 * fresh 2 MiB-aligned region. */
#define QSOE_MAP_FAILED  ((void *)-1)

void *qsoe_mmap(void *addr, unsigned long length, int prot, int flags,
                int fd, long off)
{
    (void)addr; (void)prot; (void)flags; (void)off;
    if (fd >= 0) { qsoe_errno = ENODEV; return QSOE_MAP_FAILED; }
    if (length == 0) { qsoe_errno = EINVAL; return QSOE_MAP_FAILED; }

    seL4_Word mr0 = (seL4_Word)length, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_MMAP, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return QSOE_MAP_FAILED; }
    return (void *)(unsigned long)mr0;
}
