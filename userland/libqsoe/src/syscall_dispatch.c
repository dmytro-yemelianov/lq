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

#define SYS_close       57
#define SYS_openat      56
#define SYS_lseek       62
#define SYS_read        63
#define SYS_write       64
#define SYS_readv       65
#define SYS_writev      66
#define SYS_exit        93
#define SYS_exit_group  94
#define SYS_brk        214
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

/* Forward decl of the brk implementation (real one lands in step 2). */
void *qsoe_brk(void *addr);

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
    case SYS_brk:
        /* Provided by qsoe_brk in libqsoe; landed in step 2. */
        return (long)qsoe_brk((void *)a);
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

/* qsoe_brk — the heap break primitive. spawn.c reserves a 2 MiB
 * Mega_Page mapped at CHILD_HEAP_BASE; this function tracks the
 * current break pointer within that region.
 *
 * Linux brk(addr) returns the resulting break. musl's lite_malloc
 * compares the returned value against the requested one to decide
 * whether the kernel honoured the request. brk(0) — used by sbrk(0)
 * and by lite_malloc's init path — returns the current break. */
#define QSOE_HEAP_BASE   0x800000UL
#define QSOE_HEAP_LIMIT  (QSOE_HEAP_BASE + 0x200000UL)  /* 2 MiB */

static unsigned long s_brk = QSOE_HEAP_BASE;

void *qsoe_brk(void *addr)
{
    unsigned long want = (unsigned long)addr;
    if (want == 0) return (void *)s_brk;
    if (want < QSOE_HEAP_BASE || want > QSOE_HEAP_LIMIT) {
        /* Linux semantics: out-of-range request returns the current
         * break unchanged (callers detect failure by comparing). */
        return (void *)s_brk;
    }
    s_brk = want;
    return (void *)s_brk;
}
