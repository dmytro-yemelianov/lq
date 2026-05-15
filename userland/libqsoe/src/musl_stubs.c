/*
 * libqsoe/src/musl_stubs.c — minimal QSOE-side stubs for musl-internal
 * helpers we deliberately exclude from libc.a.
 *
 * The Linux-syscall families we strip from musl (futex, sysinfo) leave
 * a handful of internal symbols referenced by code we DO keep — malloc's
 * lock, sem_timedwait, sysconf's _SC_PHYS_PAGES path, etc.  Those
 * references must resolve or every binary fails to link.
 *
 * v0.6.4 implements them as the simplest correct thing:
 *
 *   __wait / __timedwait_cp   — QSOE userland is single-threaded per
 *                                process (Stream B's true threading
 *                                lands later), so the conditions these
 *                                helpers wait on aren't observed by
 *                                another thread.  Return immediately.
 *
 *   __lsysinfo                — fills the struct with zeros + mem_unit=1
 *                                so sysconf's _SC_PHYS_PAGES /
 *                                _SC_AVPHYS_PAGES math doesn't divide
 *                                by zero.  A future TM_REQ_SYSINFO wire
 *                                request to taskman will surface real
 *                                physical-memory accounting.
 */

struct sysinfo {
    unsigned long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char __reserved[256];
};

/* Loud-stub helper.  Each stub increments a counter and prints once
 * per process to stderr.  That way a curious developer (or CI) sees
 *   QSOE-STUB: __wait hit, returning 0 (TODO: see musl_stubs.c)
 * the first time the stub is exercised, and a "this stub still hits"
 * test can grep for it.  Counters are also exposed as globals so
 * tests can assert "should never be > 0" once real impls land. */
extern int printf(const char *, ...);

int qsoe_stub_hits_lsysinfo;
int qsoe_stub_hits_wait;
int qsoe_stub_hits_timedwait_cp;

static int s_warned_lsysinfo;
static int s_warned_wait;
static int s_warned_timedwait_cp;

int __lsysinfo(struct sysinfo *info);
int __lsysinfo(struct sysinfo *info)
{
    if (++qsoe_stub_hits_lsysinfo == 1 && !s_warned_lsysinfo) {
        s_warned_lsysinfo = 1;
        printf("QSOE-STUB: __lsysinfo() hit — returning zeroed struct + mem_unit=1.\n"
               "           Replace with TM_REQ_SYSINFO when taskman tracks phys mem.\n");
    }
    if (!info)
        return -1;
    char *p = (char *)info;
    for (unsigned i = 0; i < sizeof(*info); ++i) p[i] = 0;
    info->mem_unit = 1;
    return 0;
}

/* clockid_t and struct timespec come from musl's <time.h>; we don't
 * need them for the stub bodies. */
void __wait(volatile int *addr, volatile int *waiters, int val, int priv);
void __wait(volatile int *addr, volatile int *waiters, int val, int priv)
{
    (void)addr; (void)waiters; (void)val; (void)priv;
    if (++qsoe_stub_hits_wait == 1 && !s_warned_wait) {
        s_warned_wait = 1;
        printf("QSOE-STUB: __wait() hit — returning immediately.\n"
               "           Replace when libqsoe has a real pthread surface.\n");
    }
}

int __timedwait_cp(volatile int *addr, int val, int clk,
                   const void *at, int priv);
int __timedwait_cp(volatile int *addr, int val, int clk,
                   const void *at, int priv)
{
    (void)addr; (void)val; (void)clk; (void)at; (void)priv;
    if (++qsoe_stub_hits_timedwait_cp == 1 && !s_warned_timedwait_cp) {
        s_warned_timedwait_cp = 1;
        printf("QSOE-STUB: __timedwait_cp() hit — returning 0 immediately.\n"
               "           Replace when libqsoe has a real futex/pthread surface.\n");
    }
    return 0;
}
