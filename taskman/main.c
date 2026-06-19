/*
 * taskman — central system server (QSOE).
 *
 * main.c owns the dispatch loop (Recv → handler → ReplyRecv).  The
 * handlers themselves live in subsystem dirs:
 *   proc/  — processes / threads / channels / connections / pulses
 *   mem/   — mmap
 *   path/  — pathmgr + cpiofs + open/close/io
 *   sys/   — console (and other system services)
 *
 * v0.7 split: server.{c,h} retired; each handler imported from its
 * subsystem header.
 */

#include "sel4_types.h"
#include "sel4_syscalls.h"
#include "qsoe_invoke.h"
#include <tm_log.h>
#include "tm_kdbg.h"

#include "proc/proc.h"
#include "proc/spawn.h"
#include "mem/mem.h"
#include "path/path.h"
#include "path/pathmgr.h"
#include "path/cpiofs.h"
#include "path/sysfs.h"
#include "path/procfs.h"
#include "sys/console.h"
#include "sys/fdt.h"
#include "sys/irq.h"
#include "sys/platform.h"
#include "sys/rsrcdb.h"
#include "sys/syscfg.h"
#include "sys/sysmap.h"

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include <qsoe/sysinfo.h>
#include <qsoe/sys_version.h>
#include <cpio.h>
#include <limits.h>     /* CHAR_BIT, UCHAR_MAX -- thread-name byte unpack */

/* LQ's variant-private wire opcodes must live in the variant space
 * (>= TM_REQ_VARIANT_BASE) so they can never collide with a shared
 * opcode -- see the rule in <qsoe/tm_msgs.h>. */
_Static_assert(TM_REQ_DUP_CAP            >= TM_REQ_VARIANT_BASE &&
               TM_REQ_DETACH_CAP         >= TM_REQ_VARIANT_BASE &&
               TM_REQ_CHANNEL_BIND_THREAD >= TM_REQ_VARIANT_BASE &&
               TM_REQ_THREAD_SETNAME      >= TM_REQ_VARIANT_BASE,
               "LQ variant opcode defined below TM_REQ_VARIANT_BASE");

/* TM_REQ_THREAD_SETNAME packs the whole thread name into MR1..MR2. */
_Static_assert(TM_THREAD_NAME_LEN == 2 * sizeof(seL4_Word),
               "thread name no longer packs into exactly MR1..MR2");

#ifdef TM_USE_INITRD_LOADER
/* Vestigial FDT-driven initrd loader.  See sys/initrd.c top-of-file
 * note and taskman/Makefile.  Default build path embeds the
 * modpkg.cpio via .incbin (_userland_cpio_start[]). */
#  include "sys/initrd.h"
#else
extern const char _userland_cpio_start[];
extern const char _userland_cpio_end[];
#endif

/* Emit one QSOE_SYSINFO_THREADS record into the reply buffer, honoring
 * the caller's skip/want window.  *idx is the running record ordinal
 * (advanced for every candidate, emitted or skipped); *got counts those
 * actually written.  A NULL/empty name leaves the field zeroed. */
static void
si_emit_thread(unsigned char *dst, unsigned recsz, unsigned want,
               unsigned skip, unsigned *idx, unsigned *got,
               int tid, int pid, char state, const char *name)
{
    if (*got >= want) return;
    if ((*idx)++ < skip) return;
    qsoe_sysinfo_thread_t t;
    unsigned char *tb = (unsigned char *)&t;
    for (unsigned b = 0; b < recsz; ++b) tb[b] = 0;
    t.tid    = (int32_t)tid;
    t.pid    = (int32_t)pid;
    t.hartid = 0;
    t.state  = state;
    if (name)
        for (unsigned n = 0; n < QSOE_SYSINFO_NAME_LEN - 1 &&
                             name[n] != '\0'; ++n)
            t.name[n] = name[n];
    for (unsigned b = 0; b < recsz; ++b)
        dst[*got * recsz + b] = tb[b];
    ++*got;
}

/* Dispatch one incoming message. */
static seL4_MessageInfo_t
tm_dispatch(seL4_MessageInfo_t info, seL4_Word badge,
            seL4_Word mr0, seL4_Word mr1, seL4_Word mr2, seL4_Word mr3,
            seL4_Word *out_mr0, seL4_Word *out_mr1,
            seL4_Word *out_mr2, seL4_Word *out_mr3,
            int *out_no_reply)
{
    pid_t caller = (pid_t)badge;
    unsigned label = (unsigned)seL4_MessageInfo_get_label(info);
    /* Mirror the request's first four message registers into the IPC
     * buffer so a handler can read the whole request contiguously from
     * msg[0..] (words 0-3 here, words 4+ already in the buffer).  Existing
     * handlers that read mr0..mr3 as parameters are unaffected -- this is
     * purely additive, and it lets pure-payload handlers (e.g. readlink)
     * receive a variable-length argument right after the header word. */
    qsoe_ipcbuf->msg[0] = mr0;
    qsoe_ipcbuf->msg[1] = mr1;
    qsoe_ipcbuf->msg[2] = mr2;
    qsoe_ipcbuf->msg[3] = mr3;
    /* --debug=4 (TRACE): one short line per incoming message so an operator
     * can see how IPC-heavy an operation is, e.g. "tm_msg 0x302". */
    tm_trace("tm_msg 0x%x\n", label);
    seL4_Word err = 0;
    seL4_Word reply_len = 0;
    *out_mr0 = 0;
    *out_mr1 = 0;
    *out_mr2 = 0;
    *out_mr3 = 0;
    *out_no_reply = 0;

    /* Hybrid timer expiry: every dispatch entry walks the timer
     * state, wakes any expired nanosleep callers, delivers SIGALRM
     * pulses for expired ITIMER_REALs.  Quiet systems see coarse
     * granularity; IPC-busy ones see real-time timer behaviour. */
    tm_timer_sweep();

    switch (label) {
    /* ---------- system management ---------- */
    case TM_REQ_CLOCK_FREQ: {
        /* RISC-V `time` CSR frequency.  v0.8 reads it from the FDT
         * via syscfg; falls back to the qemu-virt hardcode if the
         * blob hasn't been built (i.e. FDT not available at boot —
         * shouldn't happen on supported platforms, but the fallback
         * keeps single-process tests booting). */
        uint64_t hz = 0;
        if (tm_syscfg_find_u64(TM_SYSCFG_TAG_TIMEBASE_HZ, &hz) != 0 ||
            hz == 0) {
            hz = TM_CLOCK_FREQ_HZ;
        }
        *out_mr0 = (seL4_Word)hz;
        reply_len = 1;
        break;
    }
    case TM_REQ_IRQ_ATTACH: {
        /* mr0 = PLIC IRQ number, mr1 = trigger.  Reply mr0 = handler
         * slot, mr1 = ntfn slot — both in the caller's CSpace. */
        seL4_CPtr h = 0, n = 0;
        int rc = tm_irq_attach(caller, (unsigned)mr0, (unsigned)mr1,
                                &h, &n);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)h;
        *out_mr1 = (seL4_Word)n;
        reply_len = 2;
        break;
    }
    case TM_REQ_IRQ_DETACH: {
        int rc = tm_irq_detach(caller, (seL4_CPtr)mr0, (seL4_CPtr)mr1);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_RSRC_CREATE: {
        int rc = tm_rsrc_create(caller, (unsigned)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_RSRC_DESTROY: {
        int rc = tm_rsrc_destroy(caller, (unsigned)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_RSRC_ATTACH: {
        int rc = tm_rsrc_attach(caller, (unsigned)mr0);
        if (rc) { err = (seL4_Word)(-rc); break; }
        /* Echo granted ranges back: tm_rsrc_attach mutated msg[4..]
         * in place; reply length covers the MR0..3 quad plus the
         * payload (one rsrc_request_t = 48 bytes per entry). */
        *out_mr0 = mr0;
        unsigned bytes = (unsigned)mr0 * 48u;  /* sizeof rsrc_request_t */
        reply_len = 4 + (bytes + 7) / 8;
        break;
    }
    case TM_REQ_RSRC_DETACH: {
        int rc = tm_rsrc_detach(caller, (unsigned)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_RSRC_QUERY: {
        unsigned written = 0;
        int rc = tm_rsrc_query(caller, (unsigned)mr0, (unsigned)mr1,
                                (uint32_t)mr2, &written);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)written;
        unsigned bytes = written * 32u;  /* sizeof rsrc_alloc_t */
        reply_len = 4 + (bytes + 7) / 8;
        break;
    }
    case TM_REQ_GET_SYSCFG: {
        /* Hand back the whole syscfg blob in one shot.  Caller's mr0 is
         * the max bytes it can accept; we copy min(blob_len, mr0) into
         * msg[4..] and reply with mr0 = bytes copied. */
        const void *blob; unsigned blob_len;
        if (tm_syscfg_get(&blob, &blob_len) != 0) {
            err = ENOSYS;
            break;
        }
        unsigned want = (unsigned)mr0;
        if (want > blob_len) want = blob_len;
        unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
        const unsigned char *src = (const unsigned char *)blob;
        for (unsigned i = 0; i < want; ++i) dst[i] = src[i];
        *out_mr0 = (seL4_Word)want;
        /* seL4 only copies `length` words of the IPC buffer across the
         * Call boundary.  Header (mr0..mr3) is 4 words; payload bytes
         * round up to whole words.  Without this, the client sees only
         * mr0 and reads stale data from its own IPC buffer. */
        reply_len = 4 + (want + 7) / 8;
        break;
    }
    case TM_REQ_SYSINFO: {
        /* Live system-state snapshots for sysinfo(1).  Populated from
         * taskman's own tables + syscfg.  Groups taskman cannot yet
         * source (per-hart idle ticks, IRQ fire counts, RAM accounting)
         * report empty rather than the default echo's garbage.
         *   Request: mr0 = group, mr1 = max records this window,
         *            mr2 = records to skip first.
         *   Reply:   mr0 = records available, mr1 = records returned,
         *            payload (the records) at msg[4..]. */
        unsigned group = (unsigned)mr0;
        unsigned want  = (unsigned)mr1;
        unsigned skip  = (unsigned)mr2;
        unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
        unsigned avail = 0, got = 0, recsz = 0;

        if (group == QSOE_SYSINFO_CPUS) {
            recsz = sizeof(qsoe_sysinfo_cpu_t);
            uint32_t ncpus = 0, boot = 0;
            if (tm_syscfg_find_u32(TM_SYSCFG_TAG_NUM_CPUS, &ncpus) != 0 ||
                ncpus == 0)
                ncpus = 1;
            (void)tm_syscfg_find_u32(TM_SYSCFG_TAG_BOOT_HART, &boot);
            avail = ncpus;
            for (unsigned i = skip; i < avail && got < want; ++i) {
                qsoe_sysinfo_cpu_t c;
                c.hartid      = boot + i;
                c.online      = 1;
                c.timer_ticks = 0;   /* seL4 owns the timer; no idle split */
                c.idle_ticks  = 0;
                const unsigned char *s = (const unsigned char *)&c;
                for (unsigned b = 0; b < recsz; ++b)
                    dst[got * recsz + b] = s[b];
                ++got;
            }
        } else if (group == QSOE_SYSINFO_THREADS) {
            /* One record per thread: each process's main thread (tid 1,
             * which lives in tm_process_t, not g_threads) plus every
             * ThreadCreate'd thread (the per-process system/signal thread
             * and any workers) from g_threads.  This is what makes ps(1)
             * report THR=2 for the main+signal pair. */
            recsz = sizeof(qsoe_sysinfo_thread_t);
            tm_thread_t *threads = tm_threads_array();
            for (int i = 0; i < TM_MAX_PROCESSES; ++i) {
                tm_process_t *p = tm_process_by_index(i);
                if (p && p->in_use) ++avail;
            }
            for (int i = 0; i < TM_MAX_THREADS; ++i)
                if (threads[i].in_use) ++avail;

            unsigned idx = 0;
            for (int i = 0; i < TM_MAX_PROCESSES; ++i) {
                tm_process_t *p = tm_process_by_index(i);
                if (!p || !p->in_use) continue;
                /* Main thread: named after the process, like the prior
                 * one-row-per-process output. */
                /* Detached daemons (procmgr_detach -> exit_state==1) are
                 * alive and serving; only a terminated process is a real
                 * zombie.  seL4 doesn't surface the main thread's scheduler
                 * state to taskman, so a live attached process reads as
                 * RUNNING and a detached one as DETACHED ('d'). */
                char pstate = (p->exit_state >= TM_EXIT_ZOMBIE)
                                  ? QSOE_TSTATE_ZOMBIE
                              : (p->exit_state == TM_EXIT_DETACHED)
                                  ? QSOE_TSTATE_DETACHED
                                  : QSOE_TSTATE_RUNNING;
                si_emit_thread(dst, recsz, want, skip, &idx, &got,
                               /*tid=*/1, (int)p->pid, pstate,
                               p->main_name[0] ? p->main_name : p->name);
                /* Then this process's ThreadCreate'd threads. */
                for (int j = 0; j < TM_MAX_THREADS; ++j) {
                    if (!threads[j].in_use || threads[j].pid != p->pid)
                        continue;
                    const char *nm = threads[j].name[0] ? threads[j].name
                                                        : p->name;
                    si_emit_thread(dst, recsz, want, skip, &idx, &got,
                                   threads[j].tid, (int)p->pid,
                                   QSOE_TSTATE_RUNNING, nm);
                }
            }
        }
        /* MEM / IRQS / TIMERS: not yet sourced on LQ -> empty (avail 0). */

        *out_mr0  = (seL4_Word)avail;
        *out_mr1  = (seL4_Word)got;
        reply_len = 4 + (got * recsz + 7) / 8;
        break;
    }
    case TM_REQ_SHUTDOWN:
    case TM_REQ_REBOOT: {
        /* Power the machine off.  Cred-gated (euid 0; today every process
         * runs root, so the gate always passes).  On success there is NO
         * reply -- sel4_debug_halt() -> kernel halt() -> sbi_shutdown()
         * and the board is gone.  (SBI legacy has no reboot, so REBOOT
         * halts the same way for now.) */
        tm_process_t *p = tm_process_lookup(caller);
        if (p && p->cred.euid != 0) { err = EPERM; break; }
        tm_info("shutdown: powering off (requested by pid %d)", (int)caller);
        sel4_debug_halt();   /* never returns */
        err = EIO;           /* unreachable */
        break;
    }

    /* ---------- procmgr ---------- */
    case TM_REQ_CHANNEL_CREATE: {
        int chid = (int)mr0;
        unsigned flags = (unsigned)mr1;
        int creator_tid = (int)mr2;          /* the calling thread's tid */
        unsigned long recv_slot = 0;
        int eff_chid = chid;
        int rc = tm_channel_create(caller, chid, flags, creator_tid,
                                   &recv_slot, &eff_chid);
        if (rc) { err = (seL4_Word)(-rc); }
        else    { *out_mr0 = recv_slot;
                  *out_mr1 = (seL4_Word)eff_chid;   /* assigned chid if global */
                  reply_len = 2; }
        break;
    }
    case TM_REQ_CHANNEL_DESTROY: {
        int rc = tm_channel_destroy(caller, (seL4_CPtr)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_CHANNEL_BIND_THREAD: {
        int rc = tm_channel_bind_thread(caller, (int)mr0, (int)mr1);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_CONNECT_ATTACH: {
        unsigned long send_slot = 0, ntfn_slot = 0;
        int rc = tm_connect_attach(caller, (pid_t)mr0, (int)mr1,
                                    (unsigned)mr2, &send_slot, &ntfn_slot);
        if (rc) { err = (seL4_Word)(-rc); }
        else    { *out_mr0 = send_slot;       /* MR0 = send cap slot */
                  *out_mr1 = ntfn_slot;       /* MR1 = direct-pulse ntfn slot (0 if none) */
                  reply_len = 2; }
        break;
    }
    case TM_REQ_CONNECT_DETACH: {
        int rc = tm_connect_detach(caller, (seL4_CPtr)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_CONNECT_SERVER_INFO: {
        pid_t server_pid = 0; int server_chid = 0; seL4_Word scoid = 0;
        int rc = tm_connect_server_info(caller, (seL4_CPtr)mr0,
                                         &server_pid, &server_chid, &scoid);
        if (rc) { err = (seL4_Word)(-rc); }
        else {
            *out_mr0 = (seL4_Word)server_pid;
            *out_mr1 = (seL4_Word)server_chid;
            *out_mr2 = scoid;
            reply_len = 3;
        }
        break;
    }
    case TM_REQ_CONNECT_CLIENT_INFO: {
        pid_t client_pid = 0, sid = 0; unsigned cflags = 0;
        int rc = tm_connect_client_info((seL4_Word)mr0,
                                         &client_pid, &sid, &cflags);
        if (rc) { err = (seL4_Word)(-rc); }
        else {
            *out_mr0 = (seL4_Word)client_pid;
            *out_mr1 = (seL4_Word)sid;
            *out_mr2 = (seL4_Word)cflags;
            reply_len = 3;
        }
        break;
    }
    case TM_REQ_CONNECT_FLAGS: {
        unsigned old = 0;
        int rc = tm_connect_flags(caller, (seL4_CPtr)mr0,
                                   (unsigned)mr1, (unsigned)mr2, &old);
        if (rc) { err = (seL4_Word)(-rc); }
        else    { *out_mr0 = (seL4_Word)old; reply_len = 1; }
        break;
    }
    case TM_REQ_THREAD_ALLOC: {
        int new_tid = 0;
        seL4_CPtr tcb_slot = 0, ntfn_slot = 0, reply_slot = 0;
        unsigned prio_byte = (unsigned)(mr3 & 0xffu);
        unsigned affinity  = (unsigned)((mr3 >> 8) & 0xffu);
        int rc = tm_thread_alloc(caller,
                                  (unsigned long)mr0, (unsigned)mr1,
                                  (unsigned long)mr2,
                                  prio_byte, affinity,
                                  &new_tid, &tcb_slot, &ntfn_slot,
                                  &reply_slot);
        if (rc) { err = (seL4_Word)(-rc); }
        else {
            /* MCS: mr3 carries the worker's own reply object slot (one
             * per thread — reply objects can't be shared). */
            *out_mr0 = (seL4_Word)tcb_slot;
            *out_mr1 = (seL4_Word)ntfn_slot;
            *out_mr2 = (seL4_Word)new_tid;
            *out_mr3 = (seL4_Word)reply_slot;
            reply_len = 4;
        }
        break;
    }
    case TM_REQ_THREAD_SETNAME: {
        /* Label a thread for ps(1) -H.  The caller's pid is the badge;
         * MR0 selects the thread; MR1..MR2 carry the name packed
         * little-endian.  Only threads taskman tracks live in
         * g_threads -- a process's main thread is named after its binary
         * and isn't here, so naming it is a benign no-op (ESRCH), which
         * libc's best-effort ThreadCtl ignores. */
        int       tid      = (int)mr0;
        seL4_Word words[2] = { mr1, mr2 };
        char nm[TM_THREAD_NAME_LEN];
        for (unsigned i = 0; i < TM_THREAD_NAME_LEN; ++i) {
            unsigned wi = i / sizeof(seL4_Word);   /* which MR holds byte i */
            unsigned bi = i % sizeof(seL4_Word);   /* byte offset within it */
            nm[i] = (char)((words[wi] >> (bi * CHAR_BIT)) & UCHAR_MAX);
        }
        nm[TM_THREAD_NAME_LEN - 1] = '\0';
        tm_thread_t *t = tm_thread_find(caller, tid);
        if (t) {
            for (unsigned i = 0; i < TM_THREAD_NAME_LEN; ++i) t->name[i] = nm[i];
            break;
        }
        /* The main thread (tid 1) lives in the process record, not in
         * g_threads, so tag it there; ps(1) -H reads it for the main row. */
        if (tid == 1) {
            tm_process_t *p = tm_process_lookup(caller);
            if (p) {
                for (unsigned i = 0; i < TM_THREAD_NAME_LEN; ++i)
                    p->main_name[i] = nm[i];
                break;
            }
        }
        err = (seL4_Word)ESRCH;
        break;
    }
    case TM_REQ_PROCESS_CREATE: {
        /* MR0=argc, MR1=envc, MR2=path_len, MR3=total_strs_bytes;
         * strings live at ipcbuf->msg[4..]. */
        int argc = (int)mr0;
        int envc = (int)mr1;
        unsigned plen = (unsigned)mr2;
        unsigned total_strs = (unsigned)mr3;

        static char  s_staging[1024];
        static const char *s_argv[16];
        static const char *s_envp[16];

        if (argc < 0 || argc > 16 || envc < 0 || envc > 16 ||
            plen == 0 || plen >= 64 ||
            total_strs > sizeof s_staging) {
            err = (seL4_Word)EINVAL;
            break;
        }
        const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
        for (unsigned i = 0; i < total_strs; ++i) s_staging[i] = (char)src[i];

        const char *path = s_staging;
        unsigned off = plen + 1;
        for (int i = 0; i < argc; ++i) {
            if (off >= total_strs) { err = (seL4_Word)EINVAL; break; }
            s_argv[i] = &s_staging[off];
            while (off < total_strs && s_staging[off] != 0) ++off;
            ++off;
        }
        if (err) break;
        for (int i = 0; i < envc; ++i) {
            if (off >= total_strs && envc > 0) { err = (seL4_Word)EINVAL; break; }
            s_envp[i] = &s_staging[off];
            while (off < total_strs && s_staging[off] != 0) ++off;
            ++off;
        }
        if (err) break;

        pid_t new_pid = 0;
        int rc = tm_process_create_by_name(path, plen,
                                            argc, s_argv,
                                            envc, s_envp,
                                            &new_pid);
        if (rc) { err = (seL4_Word)(-rc); }
        else {
            tm_process_set_parent(new_pid, caller);
            *out_mr0 = (seL4_Word)new_pid;
            reply_len = 1;
        }
        break;
    }
    case TM_REQ_PROCESS_TERMINATE: {
        pid_t target = (pid_t)mr0;
        if (target == 0) target = caller;
        int rc = tm_process_terminate(target, (int)mr1);
        if (rc) { err = (seL4_Word)(-rc); }
        else if (target == caller) {
            *out_no_reply = 1;
        }
        break;
    }
    case TM_REQ_PULSE_SEND: {
        int rc = tm_pulse_send(caller, (seL4_CPtr)mr0,
                                (int)mr1, (int)(int8_t)mr2, (int)mr3);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_PULSE_FETCH: {
        tm_pulse_t p; int scoid = 0;
        int rc = tm_pulse_fetch(caller, (seL4_CPtr)mr0, &p, &scoid);
        if (rc) { err = (seL4_Word)(-rc); }
        else {
            *out_mr0 = (seL4_Word)(int)p.code;
            *out_mr1 = (seL4_Word)p.value;
            *out_mr2 = (seL4_Word)p.sender_pid;
            *out_mr3 = (seL4_Word)scoid;
            reply_len = 4;
        }
        break;
    }
    case TM_REQ_PROC_DETACH: {
        int rc = tm_process_detach(caller, (int)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_WAITPID: {
        pid_t child = (pid_t)mr0;
        int status = 0; int parked = 0;
        int rc = tm_process_waitpid(caller, child, &status, &parked);
        if (rc) {
            err = (seL4_Word)(-rc);
        } else if (parked) {
            *out_no_reply = 1;
        } else {
            *out_mr0 = (seL4_Word)(unsigned)status;
            reply_len = 1;
        }
        break;
    }
    case TM_REQ_REGISTER_SIGNAL_CHID: {
        tm_process_t *proc = tm_process_lookup(caller);
        if (!proc) { err = (seL4_Word)ESRCH; break; }
        proc->signal_chid = (int)mr0;
        break;
    }
    case TM_REQ_GET_SIGNAL_CHID: {
        pid_t target = (pid_t)mr0;
        tm_process_t *proc = tm_process_lookup(target);
        if (!proc || proc->signal_chid == 0) {
            err = (seL4_Word)ESRCH;
            break;
        }
        /* Reply word order matches the shared kill.c + NQ handler:
         * mr0 = signal chid, mr1 = owner pid (ConnectAttach's (pid,
         * chid) pair check wants both).  These were swapped, so kill()
         * connected to (pid=chid, chid=pid) and the pulse missed. */
        *out_mr0 = (seL4_Word)proc->signal_chid;
        *out_mr1 = (seL4_Word)target;
        reply_len = 2;
        break;
    }
    case TM_REQ_CHDIR: {
        int rc = tm_chdir(caller, (unsigned)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_GETCWD: {
        unsigned len = 0;
        int rc = tm_getcwd(caller, &len);
        if (rc) { err = (seL4_Word)(-rc); break; }
        /* Pure-payload reply: length @ word0, cwd bytes @ word1.. (tm_getcwd
         * wrote them to msg[1..]).  Lift words 1..3 into MR1..3 so the client
         * sees [len, bytes] contiguous from word0; any tail is in msg[4..]. */
        *out_mr0 = (seL4_Word)len;
        *out_mr1 = qsoe_ipcbuf->msg[1];
        *out_mr2 = qsoe_ipcbuf->msg[2];
        *out_mr3 = qsoe_ipcbuf->msg[3];
        reply_len = 1 + (len + 7) / 8;
        break;
    }
    case TM_REQ_DUP_CAP: {
        int rc = tm_dup_cap(caller, (seL4_CPtr)mr0, (seL4_CPtr)mr1);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_UMASK: {
        /* MR0 = new mask (-1 means "query only").  Reply MR0 = old mask. */
        unsigned old = 0;
        int set = (int)(long)mr0;
        int rc = tm_umask(caller, set, &old);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)old;
        reply_len = 1;
        break;
    }
    case TM_REQ_PIPE_CREATE: {
        seL4_CPtr rfd = 0, wfd = 0;
        int rc = tm_pipe_create(caller, &rfd, &wfd);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)rfd;
        *out_mr1 = (seL4_Word)wfd;
        reply_len = 2;
        break;
    }
    case TM_REQ_DETACH_CAP: {
        /* Step 2 of two-step close: libc has already notified the
         * resmgr; we now delete the cap from the caller's CSpace
         * and free the connection-table entry. */
        int rc = tm_connect_detach(caller, (seL4_CPtr)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_NANOSLEEP: {
        /* MR0 = total nanoseconds.  Park (stash the reply object) until
         * the timer sweep wakes us. */
        int parked = 0;
        int rc = tm_nanosleep(caller, (unsigned long)mr0, &parked);
        if (rc) { err = (seL4_Word)(-rc); break; }
        if (parked) {
            *out_no_reply = 1;
        }
        break;
    }
    case TM_REQ_SETITIMER: {
        /* MR0 = which, MR1 = initial value (us), MR2 = interval (us). */
        unsigned long old_val = 0, old_int = 0;
        int rc = tm_setitimer(caller, (int)mr0,
                              (unsigned long)mr1, (unsigned long)mr2,
                              &old_val, &old_int);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)old_val;
        *out_mr1 = (seL4_Word)old_int;
        reply_len = 2;
        break;
    }
    case TM_REQ_SPAWN: {
        /* posix_spawn side channel: caller staged a packed blob
         * (path\0 argv...\0 envp...\0) into an mmap'd page; we pull it
         * back through tm_spawn_read_args, unpack it onto the stack,
         * and bottom out into tm_process_create_by_name -- the same
         * primitive TM_REQ_PROCESS_CREATE uses.  Reply mr0 = child pid.
         *
         *   MR0 = caller-VA of the args page (mmap'd by caller).
         *   MR1 = packed payload length in bytes (<= 4 KiB).
         *   MR2 = argc, MR3 = envc.
         */
        unsigned long args_va = (unsigned long)mr0;
        unsigned      args_len = (unsigned)mr1;
        int           argc     = (int)mr2;
        int           envc     = (int)mr3;

        static char        s_args_buf[4096];
        static const char *s_argv[16];
        static const char *s_envp[16];

        if (argc < 0 || argc > 16 || envc < 0 || envc > 16 ||
            args_len == 0 || args_len > sizeof s_args_buf) {
            err = (seL4_Word)EINVAL;
            break;
        }
        tm_process_t *proc = tm_process_lookup(caller);
        if (!proc) { err = (seL4_Word)ESRCH; break; }

        int rc = tm_spawn_read_args(proc, args_va, args_len, s_args_buf);
        if (rc) { err = (seL4_Word)(-rc); break; }

        /* Path: NUL-terminated string at the head of the blob. */
        unsigned plen = 0;
        while (plen < args_len && s_args_buf[plen] != 0) ++plen;
        if (plen == 0 || plen >= args_len) {
            err = (seL4_Word)EINVAL; break;
        }
        const char *path = s_args_buf;
        unsigned off = plen + 1;

        int bad = 0;
        for (int i = 0; i < argc; ++i) {
            if (off >= args_len) { bad = 1; break; }
            s_argv[i] = &s_args_buf[off];
            while (off < args_len && s_args_buf[off] != 0) ++off;
            if (off >= args_len) { bad = 1; break; }
            ++off;
        }
        if (bad) { err = (seL4_Word)EINVAL; break; }
        for (int i = 0; i < envc; ++i) {
            if (off >= args_len) { bad = 1; break; }
            s_envp[i] = &s_args_buf[off];
            while (off < args_len && s_args_buf[off] != 0) ++off;
            if (off >= args_len) { bad = 1; break; }
            ++off;
        }
        if (bad) { err = (seL4_Word)EINVAL; break; }

        pid_t new_pid = 0;
        rc = tm_process_create_by_name(path, plen, argc, s_argv,
                                        envc, s_envp, &new_pid);
        if (rc) { err = (seL4_Word)(-rc); break; }

        tm_process_set_parent(new_pid, caller);
        *out_mr0 = (seL4_Word)new_pid;
        reply_len = 1;
        break;
    }
    case TM_REQ_SET_CRED: {
        /* mr0 = ruid<<32|euid, mr1 = suid<<32|rgid, mr2 = egid<<32|sgid.
         * 0xFFFFFFFF in any 32-bit field means "no change". */
        unsigned ruid = (unsigned)(mr0 & 0xFFFFFFFFu);
        unsigned euid = (unsigned)((mr0 >> 32) & 0xFFFFFFFFu);
        unsigned suid = (unsigned)(mr1 & 0xFFFFFFFFu);
        unsigned rgid = (unsigned)((mr1 >> 32) & 0xFFFFFFFFu);
        unsigned egid = (unsigned)(mr2 & 0xFFFFFFFFu);
        unsigned sgid = (unsigned)((mr2 >> 32) & 0xFFFFFFFFu);
        int rc = tm_set_cred(caller, ruid, euid, suid, rgid, egid, sgid);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_SCHED_SET: {
        /* mr0 = pid (0 = caller), mr1 = tid, mr2 = policy, mr3 = priority.
         * The client resolves tid==0 to its own tid before sending. */
        int rc = tm_sched_set(caller, (pid_t)mr0, (int)mr1,
                              (int)mr2, (int)mr3);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_SCHED_GET: {
        /* mr0 = pid (0 = caller), mr1 = tid.  Reply: mr0 = priority,
         * mr1 = policy. */
        int policy = 0, prio = 0;
        int rc = tm_sched_get(caller, (pid_t)mr0, (int)mr1, &policy, &prio);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)prio;
        *out_mr1 = (seL4_Word)policy;
        reply_len = 2;
        break;
    }
    case TM_REQ_PROC_SELF_INFO: {
        /* v0.7: backs POSIX getpid/getppid/getuid/etc.  Caller's pid
         * is in the badge; no MR inputs.  Reply layout (8 32-bit
         * fields, packed two per 64-bit MR):
         *   mr0 = pid     | (ppid << 32)
         *   mr1 = ruid    | (euid << 32)
         *   mr2 = suid    | (rgid << 32)
         *   mr3 = egid    | (sgid << 32)
         */
        pid_t pid = 0, ppid = 0;
        struct _cred_info cred;
        int rc = tm_proc_self_info(caller, &pid, &ppid, &cred);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = ((seL4_Word)(uint32_t)pid)  | ((seL4_Word)(uint32_t)ppid      << 32);
        *out_mr1 = ((seL4_Word)cred.ruid)      | ((seL4_Word)cred.euid           << 32);
        *out_mr2 = ((seL4_Word)cred.suid)      | ((seL4_Word)cred.rgid           << 32);
        *out_mr3 = ((seL4_Word)cred.egid)      | ((seL4_Word)cred.sgid           << 32);
        reply_len = 4;
        break;
    }

    /* ---------- memmgr ---------- */
    case TM_REQ_MMAP: {
        /* mr0 = length, mr1 = flags, mr2 = phys (if flags & PHYS). */
        unsigned long base = 0;
        int rc = tm_mmap_serve(caller, (unsigned long)mr0,
                                (unsigned long)mr1, (unsigned long)mr2,
                                &base);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)base;
        reply_len = 1;
        break;
    }
    case TM_REQ_MUNMAP: {
        /* mr0 = vaddr (Mega-aligned), mr1 = length in bytes. */
        int rc = tm_munmap_serve(caller, (unsigned long)mr0,
                                  (unsigned long)mr1);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_MPROTECT: {
        /* mr0 = addr (page-aligned), mr1 = length, mr2 = PROT_* bits. */
        int rc = tm_mprotect_serve(caller, (unsigned long)mr0,
                                   (unsigned long)mr1, (unsigned long)mr2);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_ALLOC_PHYS: {
        /* mr0 = length (bytes, <= one Mega_Page), mr1 = prot.
         * Reply: mr0 = VA, mr1 = PA. */
        unsigned long va = 0, pa = 0;
        int rc = tm_alloc_phys_serve(caller, (unsigned long)mr0,
                                     (unsigned)mr1, &va, &pa);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)va;
        *out_mr1 = (seL4_Word)pa;
        reply_len = 2;
        break;
    }

    case TM_REQ_MSG_XFER: {
        /* Bulk-IPC bounce copy (doc/plans/bulk_ipc.txt).  The calling
         * SERVER drives PULL (during MsgReceive) and PUSH (during
         * MsgReply); `caller` is the server, mr1 is the connection badge
         * (scoid) the server received -- resolve it to the blocked
         * client's pid via the connection registry. */
        unsigned dir       = (unsigned)mr0;
        pid_t    client_pid = tm_connection_client_pid((seL4_Word)mr1);
        if (client_pid == 0) { err = (seL4_Word)ESRCH; break; }
        long r;
        if (dir == TM_MSG_XFER_PULL) {
            unsigned long client_src = (unsigned long)mr2;   /* client send buf */
            unsigned long server_dst = (unsigned long)mr3;   /* server recv buf */
            unsigned long len     = (unsigned long)qsoe_ipcbuf->msg[4];
            unsigned long crbuf   = (unsigned long)qsoe_ipcbuf->msg[5];
            unsigned long crbytes = (unsigned long)qsoe_ipcbuf->msg[6];
            r = tm_msg_xfer_pull(caller, client_pid, client_src, server_dst,
                                 len, crbuf, crbytes);
        } else {
            unsigned long server_src = (unsigned long)mr2;   /* server reply buf */
            unsigned long len        = (unsigned long)mr3;
            r = tm_msg_xfer_push(caller, client_pid, server_src, len);
        }
        if (r < 0) { err = (seL4_Word)(-r); break; }
        *out_mr0 = (seL4_Word)r;        /* bytes copied */
        reply_len = 1;
        break;
    }

    /* ---------- pathmgr / IO ---------- */
    case TM_REQ_OPEN: {
        seL4_CPtr slot = 0;
        int is_external = 0;
        unsigned rwlen = 0;
        int rc = tm_io_open(caller, (unsigned)mr0, &slot, &is_external, &rwlen);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = slot;
        *out_mr1 = (seL4_Word)is_external;   /* libc sends _IO_CONNECT if set */
        *out_mr2 = (seL4_Word)rwlen;         /* symlink-rewritten path len, 0 = unchanged */
        /* When a symlink rewrote the path, it sits in msg[4..]; the reply
         * must cover those words (the +4 accounts for the register MRs). */
        reply_len = (rwlen > 0) ? (4 + (rwlen + 7) / 8) : 3;
        break;
    }
    case TM_REQ_CLOSE: {
        /* libc sends this on the fd's own cap; we arrive via the
         * resmgr's channel.  Dispatch to the in-taskman resmgr's
         * per-fd close hook (cpiofs frees its dir-slot; console is a
         * no-op).  External resmgrs handle their own TM_REQ_CLOSE on
         * their own channels — taskman never sees those. */
        int rc = tm_io_close(caller, badge);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_IO_WRITE: {
        unsigned wrote = 0;
        int rc = tm_io_write(caller, badge, (unsigned)mr0, &wrote);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)wrote;
        reply_len = 1;
        break;
    }
    case TM_REQ_IO_READ: {
        unsigned got = 0;
        int rc = tm_io_read(caller, badge, (unsigned)mr0, &got);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)got;
        /* The byte payload lives in msg[4..]; the kernel only
         * transfers msg[4..length-1] across IPC, so the reply
         * length needs to cover those words (the +4 accounts for
         * the four register-passed MRs). */
        reply_len = 4 + (got + 7) / 8;
        break;
    }
    case TM_REQ_UNLINK: {
        int rc = tm_unlink(caller, (unsigned)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_FSTAT: {
        unsigned bytes = 0;
        int rc = tm_fstat(caller, badge, &bytes);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)bytes;
        reply_len = 4 + (bytes + 7) / 8;
        break;
    }
    case TM_REQ_READLINK: {
        unsigned bytes = 0;
        int rc = tm_readlink(caller, (unsigned)mr0, &bytes);
        if (rc) { err = (seL4_Word)(-rc); break; }
        /* Pure-payload reply: length @ word0, target @ word1.. (tm_readlink
         * wrote the target to msg[1..]).  Lift words 1..3 into MR1..3 so
         * the client sees [length, target] contiguous from word 0; any tail
         * past word 3 is already in msg[4..]. */
        *out_mr0  = (seL4_Word)bytes;
        *out_mr1  = qsoe_ipcbuf->msg[1];
        *out_mr2  = qsoe_ipcbuf->msg[2];
        *out_mr3  = qsoe_ipcbuf->msg[3];
        reply_len = 1 + (bytes + 7) / 8;
        break;
    }
    case TM_REQ_LSEEK: {
        /* libressrv tm_req_io_lseek_t order: MR0 = signed 64-bit offset,
         * MR1 = whence (was swapped pre-unification). */
        long off = 0;
        int rc = tm_lseek(caller, badge, (int)mr1, (long)mr0, &off);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)off;
        reply_len = 1;
        break;
    }
    case TM_REQ_READDIR: {
        unsigned bytes = 0;
        int rc = tm_readdir(caller, badge, &bytes);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)bytes;
        reply_len = 4 + (bytes + 7) / 8;
        break;
    }
    case TM_REQ_ACCESS: {
        int rc = tm_access(caller, (unsigned)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_PATHMGR_REGISTER: {
        unsigned plen = (unsigned)mr0;
        int chid = (int)mr1;
        if (plen == 0 || plen >= 128) { err = (seL4_Word)EINVAL; break; }
        static char s_reg_path[128];
        const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
        for (unsigned i = 0; i < plen; ++i) s_reg_path[i] = (char)src[i];
        s_reg_path[plen] = 0;

        tm_pathmgr_obj_t obj = {
            .server_pid   = caller,
            .server_chid  = chid,
            .flags        = 0,
            .handler_kind = PATHMGR_HANDLER_EXTERNAL,
        };
        int rc = tm_pathmgr_register(s_reg_path, &obj);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_PATHMGR_REPATH: {
        unsigned plen = (unsigned)mr0;
        if (plen == 0 || plen >= 128) { err = (seL4_Word)EINVAL; break; }
        static char s_repath[128];
        const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
        for (unsigned i = 0; i < plen; ++i) s_repath[i] = (char)src[i];
        s_repath[plen] = 0;

        tm_pathmgr_obj_t obj = {
            .server_pid   = (pid_t)mr1,
            .server_chid  = (int)mr2,
            .flags        = 0,
            .handler_kind = (unsigned)mr3,
        };
        int rc = tm_pathmgr_repath(s_repath, &obj);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }

    case TM_REQ_PATHMGR_RESOLVE: {
        unsigned plen = (unsigned)mr0;
        if (plen == 0 || plen >= 128) { err = (seL4_Word)EINVAL; break; }
        static char s_qpath[128];
        const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
        for (unsigned i = 0; i < plen; ++i) s_qpath[i] = (char)src[i];
        s_qpath[plen] = 0;

        tm_pathmgr_obj_t obj = { 0 };
        unsigned consumed = 0;
        int rc = tm_pathmgr_resolve(s_qpath, &obj, &consumed);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = (seL4_Word)obj.server_pid;
        *out_mr1 = (seL4_Word)obj.server_chid;
        *out_mr2 = (seL4_Word)obj.handler_kind;
        *out_mr3 = (seL4_Word)consumed;
        reply_len = 4;
        break;
    }

    default:
        /* Application-level message (v0.3 demo). Echo back with MR0
         * incremented, so tester sees the round-trip succeed. */
        *out_mr0 = mr0 + 1;
        reply_len = 1;
        break;
    }
    return seL4_MessageInfo_new(err, 0, 0, reply_len);
}

#define QSOE_STR_(x) #x
#define QSOE_STR(x)  QSOE_STR_(x)
#define QSOE_VSHORT  "v" QSOE_STR(QSOE_VERSION_MAJOR) "." QSOE_STR(QSOE_VERSION_MINOR)

static void print_banner(void)
{
    unsigned long i;
    const char banner_str[] = "QSOE/L Operating System version ";
    unsigned long n = sizeof(banner_str) + sizeof(QSOE_VSHORT);

    sel4_debug_puts("\n+");
    for (i = 0; i < n; i++)
        sel4_debug_putchar('-');
    sel4_debug_puts("+\n| ");
    sel4_debug_puts(banner_str);
    sel4_debug_puts(QSOE_VSHORT " |\n+");
    for (i = 0; i < n; i++)
        sel4_debug_putchar('-');
    sel4_debug_puts("+\n\n");
}

/* Pick the master-pool untyped: the largest non-device untyped whose
 * size does NOT exceed TM_MASTER_UT_MAX_BITS.  We deliberately avoid the
 * board's biggest untypeds here: seL4 hands every boot untyped marked
 * "fully used" (MAX_FREE_INDEX), so the first retype from one lazily
 * clears the entire block.  Handing the master pool a multi-GiB untyped
 * made the primary-endpoint retype clear gigabytes (~18 s on the FU740);
 * capping the size keeps that one-time clear to ~1 s.  The big untypeds
 * remain available through the pp_ut pool, which draws them ascending so
 * a typical workload never has to clear them at all.
 *
 * Falls back to the overall largest if (pathologically) every untyped
 * exceeds the cap -- correctness over boot speed in that corner. */
static seL4_CPtr find_master_ram_untyped(seL4_BootInfo *bi)
{
    unsigned n = bi->untyped.end - bi->untyped.start;
    unsigned best_capped = (unsigned)-1, best_capped_bits = 0;
    unsigned best_any    = (unsigned)-1, best_any_bits    = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (bi->untypedList[i].isDevice) continue;
        unsigned bits = bi->untypedList[i].sizeBits;
        if (bits > best_any_bits) {
            best_any_bits = bits;
            best_any = i;
        }
        if (bits <= TM_MASTER_UT_MAX_BITS && bits > best_capped_bits) {
            best_capped_bits = bits;
            best_capped = i;
        }
    }
    unsigned best = (best_capped != (unsigned)-1) ? best_capped : best_any;
    if (best == (unsigned)-1) return 0;
    return bi->untyped.start + best;
}

static seL4_CPtr find_device_untyped_for_paddr(seL4_BootInfo *bi,
                                                unsigned long paddr)
{
    unsigned n = bi->untyped.end - bi->untyped.start;
    for (unsigned i = 0; i < n; ++i) {
        if (!bi->untypedList[i].isDevice) continue;
        unsigned long base = bi->untypedList[i].paddr;
        unsigned long size = 1UL << bi->untypedList[i].sizeBits;
        if (paddr >= base && paddr < base + size) {
            return bi->untyped.start + i;
        }
    }
    return 0;
}

/* Find the FDT blob in the extra-BI region.  seL4's kernel/boot.c
 * appends the DTB as a SEL4_BOOTINFO_HEADER_FDT chunk in the page
 * immediately following the main BootInfo frame.  Each chunk is a
 * 16-byte (id, len) header followed by `len - 16` payload bytes.
 * Returns 0 / NULL if the kernel didn't provide a DTB. */
#define SEL4_BOOTINFO_HEADER_FDT  6
typedef struct { seL4_Word id; seL4_Word len; } tm_bi_header_t;

static const void *find_fdt_in_extra_bi(seL4_BootInfo *bi,
                                         unsigned *out_size)
{
    if (!bi || bi->extraLen == 0) return 0;
    /* The kernel maps extra-BI at the page immediately after the
     * BootInfo frame.  PAGE_SIZE is 4 KiB on RISC-V. */
    const unsigned char *p = (const unsigned char *)bi + 4096;
    seL4_Word off = 0;
    while (off + sizeof(tm_bi_header_t) <= bi->extraLen) {
        const tm_bi_header_t *h = (const tm_bi_header_t *)(p + off);
        if (h->len < sizeof *h) return 0;
        if (h->id == SEL4_BOOTINFO_HEADER_FDT) {
            if (out_size) *out_size = (unsigned)(h->len - sizeof *h);
            return (const void *)(h + 1);
        }
        off += h->len;
    }
    return 0;
}

int main(seL4_BootInfo *bi)
{
    /* Point the shared logger at the seL4 debug console FIRST — before
     * this, tm_log() has no sink and drops silently.  Threshold starts
     * at TM_LOG_LEVEL_DEFAULT (INFO); --debug from the boot cmdline
     * (below, once the FDT is in hand) can raise it to DBG/TRACE. */
    tm_log_console_init();

    print_banner();
    qsoe_libc_init(bi->ipcBuffer, QSOE_PID_TASKMAN);

    /* Parse FDT (if seL4 published one) and build the syscfg blob.
     * If anything fails, syscfg-dependent handlers fall back to the
     * compile-time hardcoded values — boot still completes. */
    unsigned dtb_size = 0;
    const void *dtb = find_fdt_in_extra_bi(bi, &dtb_size);

    /* Boot command line from /chosen/bootargs.  Echo it Linux-style
     * right after the banner (which leaves a trailing blank line), then
     * honor `--debug[=N]` (same verbosity contract as NQ) -- applied
     * before the first gated log line below so the level is settled.
     * No FDT / no bootargs => default INFO. */
    const char *bootargs = 0;
    if (dtb) {
        int chosen = tm_fdt_path(dtb, "/chosen");
        if (chosen >= 0)
            (void)tm_fdt_prop_str(dtb, chosen, "bootargs", &bootargs);
    }
    tm_info("Boot command line: %s",
            (bootargs && bootargs[0]) ? bootargs : "(none)");
    if (bootargs)
        tm_log_apply_cmdline(bootargs);

    if (dtb && tm_syscfg_build(dtb) == 0) {
        tm_info("syscfg built from FDT");
        /* Translate the syscfg data into the 'PSYS' sysmap page that
         * proc/spawn maps read-only at QSOE_SYSMAP_VA in every child,
         * so the shared libc hwi_init() works on LQ exactly as on NQ. */
        if (tm_sysmap_build() == 0)
            tm_info("sysmap page built");
    } else {
        tm_info("no FDT in extra-BI; syscfg falls back");
    }

    /* Resource database — empty pool + per-class lists, then seed
     * MEMORY entries from the syscfg blob. */
    tm_rsrc_init();
    tm_rsrc_seed_from_syscfg();

    /* Pick up timebase-Hz from syscfg if available, hardcode otherwise. */
    {
        uint64_t hz = 0;
        if (tm_syscfg_find_u64(TM_SYSCFG_TAG_TIMEBASE_HZ, &hz) == 0 && hz) {
            qsoe_time_freq_hz = hz;
        } else {
            qsoe_time_freq_hz = TM_CLOCK_FREQ_HZ;
        }
    }

    seL4_CPtr ut = find_master_ram_untyped(bi);
    if (ut == 0) {
        tm_crash("no RAM untyped");
    }

    /* Per-process untyped pool: every RAM untyped at least one pp_ut
     * block wide.  Without this taskman would only ever touch the single
     * largest untyped (here 128 MiB of a 512 MiB board), exhausting it
     * after a handful of spawns while the rest sat idle.  pp_ut_acquire
     * carves blocks across the whole list.
     *
     * The master untyped (`ut`) is excluded: it is the master pool's
     * dedicated block, and the master allocation path has no
     * grow-on-exhaustion fallback, so it must not be shared with pp_ut
     * carving.  The list is in bootinfo (ascending-address) order, which
     * is effectively ascending size, so pp_ut draws the small untypeds
     * first and reaches the board's multi-GiB blocks (whose first carve
     * triggers seL4's whole-block lazy clear) only under real memory
     * pressure. */
    {
        seL4_CPtr pool[TM_RAM_UT_MAX];
        int       pool_n = 0;
        unsigned  dn = bi->untyped.end - bi->untyped.start;
        for (unsigned i = 0; i < dn && pool_n < TM_RAM_UT_MAX; ++i) {
            if (bi->untypedList[i].isDevice) continue;
            if (bi->untypedList[i].sizeBits < TM_PP_UT_BITS) continue;
            if (bi->untyped.start + i == ut) continue;   /* master's own block */
            pool[pool_n++] = bi->untyped.start + i;
        }
        tm_pput_pool_init(pool, pool_n);
    }

    seL4_CPtr uart_ut = find_device_untyped_for_paddr(bi, 0x10000000UL);
    tm_set_uart_untyped(uart_ut);
    tm_mem_set_bootinfo(bi);  /* needed by MAP_PHYS to walk device-UTs */
    if (uart_ut == 0) {
        tm_warn("no device-untyped at 0x10000000");
    }

    tm_init(ut, seL4_CapInitThreadCNode, bi->empty.start);

    seL4_CPtr primary_ep = bi->empty.start;
    s_next_slot = bi->empty.start + 1;
    seL4_Word rerr = qsoe_untyped_retype(ut, seL4_EndpointObject, 0,
                                          seL4_CapInitThreadCNode, 0, 0,
                                          primary_ep, 1);
    if (rerr != 0) {
        tm_crash("failed to retype primary endpoint");
    }

    /* MCS: record the per-core SchedControl caps (needed to give every
     * spawned thread a scheduling context — without one a TCB never
     * runs) and allocate the dispatcher's own reply object before the
     * first Recv. */
    tm_set_sched_control(bi->schedcontrol.start, bi->numNodes);
    if (tm_reply_init() != 0) {
        tm_crash("failed to allocate dispatcher reply object");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, 1,
                                      primary_ep, primary_ep) != 0) {
        tm_crash("failed to register primary channel");
    }

    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_CONSOLE_CHID,
                                      primary_ep, primary_ep) != 0) {
        tm_crash("failed to register console channel");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_CPIOFS_CHID,
                                      primary_ep, primary_ep) != 0) {
        tm_crash("failed to register cpiofs channel");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_DEVNULL_CHID,
                                      primary_ep, primary_ep) != 0) {
        tm_crash("failed to register /dev/null channel");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_DEVZERO_CHID,
                                      primary_ep, primary_ep) != 0) {
        tm_crash("failed to register /dev/zero channel");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_PMDIR_CHID,
                                      primary_ep, primary_ep) != 0) {
        tm_crash("failed to register pmdir channel");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_SYSFS_CHID,
                                      primary_ep, primary_ep) != 0) {
        tm_crash("failed to register sysfs channel");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_PROCFS_CHID,
                                      primary_ep, primary_ep) != 0) {
        tm_crash("failed to register procfs channel");
    }

    tm_pathmgr_init();
    {
        tm_pathmgr_obj_t obj = {
            .server_pid   = QSOE_PID_TASKMAN,
            .server_chid  = TM_CONSOLE_CHID,
            .flags        = 0,
            .handler_kind = PATHMGR_HANDLER_TASKMAN_CONSOLE,
        };
        if (tm_pathmgr_register("/dev/console", &obj) != 0) {
            tm_crash("pathmgr register /dev/console failed");
        }
    }
    {
        tm_pathmgr_obj_t obj = {
            .server_pid   = QSOE_PID_TASKMAN,
            .server_chid  = TM_CPIOFS_CHID,
            .flags        = 0,
            .handler_kind = PATHMGR_HANDLER_TASKMAN_CPIOFS,
        };
        if (tm_pathmgr_register("/", &obj) != 0) {
            tm_crash("pathmgr register / failed");
        }
    }

    /* /dev/tty — POSIX controlling-terminal alias.  Symlink to
     * /dev/console so repath()s on the latter (e.g. devc-ser8250
     * coming up at boot) carry over.  isatty(open("/dev/tty")) is
     * how qsh detects its tty — once this resolves, qsh's edit.c
     * editor (with arrow-key history) takes over. */
    if (tm_pathmgr_symlink("/dev/tty", "/dev/console") != 0) {
        tm_crash("pathmgr symlink /dev/tty failed");
    }

    /* Cross-fs config/home links (/etc -> /usr/conf, /home -> /usr/home)
     * are NOT registered here -- they are real symlink inodes in the boot
     * cpio (declared via `ln -sf` in the modpkg recipe), so they appear in
     * `ls -la /` and readlink/lstat work directly.  Resolution follows
     * them at open time via tm_pathmgr_expand_symlink_cpio (path/io.c).
     * The cpio is the single source of truth -- no in-memory node, so no
     * duplicate entry in the readdir merge. */

    /* /dev/null and /dev/zero — POSIX-essential pseudo-devices,
     * each backed by a trivial taskman-internal resmgr (sys/devnull.c
     * and sys/devzero.c).  Different chids so path/io.c dispatch
     * can pick the right handler. */
    {
        tm_pathmgr_obj_t obj = {
            .server_pid   = QSOE_PID_TASKMAN,
            .server_chid  = TM_DEVNULL_CHID,
            .flags        = 0,
            .handler_kind = PATHMGR_HANDLER_TASKMAN_NULL,
        };
        if (tm_pathmgr_register("/dev/null", &obj) != 0) {
            tm_crash("pathmgr register /dev/null failed");
        }
    }
    {
        tm_pathmgr_obj_t obj = {
            .server_pid   = QSOE_PID_TASKMAN,
            .server_chid  = TM_DEVZERO_CHID,
            .flags        = 0,
            .handler_kind = PATHMGR_HANDLER_TASKMAN_ZERO,
        };
        if (tm_pathmgr_register("/dev/zero", &obj) != 0) {
            tm_crash("pathmgr register /dev/zero failed");
        }
    }

    /* Synthetic /dev directory.  Without this, the implicit "dev"
     * parent node in pathmgr's tree (created by every /dev/<X>
     * registration above) has no obj → ls /dev falls back to
     * cpiofs and gets ENOENT.  PMDIR's open + readdir walk
     * pathmgr's own child list. */
    {
        tm_pathmgr_obj_t obj = {
            .server_pid   = QSOE_PID_TASKMAN,
            .server_chid  = TM_PMDIR_CHID,
            .flags        = 0,
            .handler_kind = PATHMGR_HANDLER_TASKMAN_PMDIR,
        };
        if (tm_pathmgr_register("/dev", &obj) != 0) {
            tm_crash("pathmgr register /dev failed");
        }
    }

    /* Synthetic read-only /sys ("the kernel describes itself").  The
     * model is the shared libtaskman core; tm_sysfs_populate() snapshots
     * board/version/builddate from the syscfg blob (already built above)
     * + the version header.  Backs `read BOARD < /sys/board` in init. */
    tm_sysfs_populate();
    {
        tm_pathmgr_obj_t obj = {
            .server_pid   = QSOE_PID_TASKMAN,
            .server_chid  = TM_SYSFS_CHID,
            .flags        = 0,
            .handler_kind = PATHMGR_HANDLER_TASKMAN_SYSFS,
        };
        if (tm_pathmgr_register("/sys", &obj) != 0) {
            tm_crash("pathmgr register /sys failed");
        }
    }

    /* Synthetic read-only /proc (one dir per live pid; ps reads it).
     * The model is the shared libtaskman core; tm_procfs_populate()
     * registers LQ's process-table accessors with it. */
    tm_procfs_populate();
    {
        tm_pathmgr_obj_t obj = {
            .server_pid   = QSOE_PID_TASKMAN,
            .server_chid  = TM_PROCFS_CHID,
            .flags        = 0,
            .handler_kind = PATHMGR_HANDLER_TASKMAN_PROCFS,
        };
        if (tm_pathmgr_register("/proc", &obj) != 0) {
            tm_crash("pathmgr register /proc failed");
        }
    }

    const void   *cpio_data;
    unsigned long cpio_len;
#ifdef TM_USE_INITRD_LOADER
    if (tm_initrd_load(bi, dtb, &cpio_data, &cpio_len) != 0) {
        tm_crash("initrd: tm_initrd_load failed (no -initrd? "
                 "build modpkg.cpio in ../quser).");
    }
    tm_info("initrd: %lu bytes mapped at 0x%lx",
            cpio_len, (unsigned long) cpio_data);
#else
    /* Default path: modpkg.cpio is embedded by .incbin (see
     * taskman/Makefile), so the bytes are already mapped
     * inside taskman's text image -- no FDT lookup, no untyped
     * retype, no extra page-tables. */
    cpio_data = _userland_cpio_start;
    cpio_len  = (unsigned long)(_userland_cpio_end - _userland_cpio_start);
#endif

    tm_set_userland_cpio(cpio_data, cpio_len);
    tm_set_primary_ep(primary_ep);
    tm_cpiofs_set_cpio(cpio_data, cpio_len);

    unsigned long elf_size = 0;
    const void *elf = cpio_get_file(cpio_data, cpio_len,
                                     "sbin/init", &elf_size);
    if (!elf) {
        tm_crash("sbin/init not found in CPIO");
    }
    pid_t init_pid = tm_pid_alloc();
    if (!init_pid) {
        tm_crash("pid allocator empty");
    }
    tm_info("spawning /sbin/init (pid=%ld)...", (long)init_pid);
    static const char *boot_argv0 = "init";
    const char *boot_argv[1] = { boot_argv0 };
    int sr = tm_spawn(elf, elf_size, init_pid, primary_ep,
                       /*argc=*/1, boot_argv,
                       /*envc=*/0, 0,
                       /*elf_name=*/"sbin/init");
    if (sr != 0) {
        tm_crash("tm_spawn returned non-zero");
    }
    tm_info("dispatcher ready");

    seL4_Word badge;
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    /* MCS: every Recv/ReplyRecv carries the dispatcher's active reply
     * object (register a6).  Deferred-reply handlers move it aside via
     * tm_reply_park() and a fresh one replaces it, so tm_active_reply()
     * is re-read each iteration. */
    seL4_MessageInfo_t info = qsoe_sys_recv(primary_ep, &badge,
                                             tm_active_reply(),
                                             &mr0, &mr1, &mr2, &mr3);
    for (;;) {
        /* A fault IPC (badge carries TM_FAULT_BADGE_FLAG) arrives on the
         * same primary EP as normal requests: the faulting thread is
         * blocked waiting for us.  Terminate it gracefully and Recv the
         * next message -- no reply, the faulter is gone. */
        if (badge & TM_FAULT_BADGE_FLAG) {
            tm_handle_fault((pid_t)(badge & TM_FAULT_PID_MASK),
                            (unsigned)seL4_MessageInfo_get_label(info));
            mr0 = 0; mr1 = 0; mr2 = 0; mr3 = 0;
            info = qsoe_sys_recv(primary_ep, &badge,
                                  tm_active_reply(),
                                  &mr0, &mr1, &mr2, &mr3);
            continue;
        }

        seL4_Word r0, r1, r2, r3;
        int no_reply = 0;
        seL4_MessageInfo_t reply_info = tm_dispatch(info, badge,
                                                     mr0, mr1, mr2, mr3,
                                                     &r0, &r1, &r2, &r3,
                                                     &no_reply);
        if (no_reply) {
            mr0 = 0; mr1 = 0; mr2 = 0; mr3 = 0;
            info = qsoe_sys_recv(primary_ep, &badge,
                                  tm_active_reply(),
                                  &mr0, &mr1, &mr2, &mr3);
        } else {
            mr0 = r0; mr1 = r1; mr2 = r2; mr3 = r3;
            info = qsoe_sys_reply_recv(primary_ep, reply_info, &badge,
                                        tm_active_reply(),
                                        &mr0, &mr1, &mr2, &mr3);
        }
    }
    return 0;
}
