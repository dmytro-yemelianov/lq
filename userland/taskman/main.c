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

#include "sel4_syscalls.h"
#include "sel4_types.h"
#include "qsoe_invoke.h"

#include "proc/proc.h"
#include "proc/spawn.h"
#include "mem/mem.h"
#include "path/path.h"
#include "path/pathmgr.h"
#include "path/cpiofs.h"
#include "sys/console.h"
#include "sys/platform.h"
#include "sys/syscfg.h"

#include <qsoe-system.h>
#include "../libqsoe/include/qsoe/slots.h"
#include "../libqsoe/include/qsoe/wire.h"
#include <qsoe/sys_version.h>
#include <cpio/cpio.h>

extern char _userland_cpio_start[];
extern char _userland_cpio_end[];

/* Dispatch one incoming message. */
static seL4_MessageInfo_t
tm_dispatch(seL4_MessageInfo_t info, seL4_Word badge,
            seL4_Word mr0, seL4_Word mr1, seL4_Word mr2, seL4_Word mr3,
            seL4_Word *out_mr0, seL4_Word *out_mr1,
            seL4_Word *out_mr2, seL4_Word *out_mr3,
            int *out_no_reply)
{
    (void)mr3;
    pid_t caller = (pid_t)badge;
    unsigned label = (unsigned)seL4_MessageInfo_get_label(info);
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
    /* ---------- sysmgr ---------- */
    case TM_REQ_DEBUG_SLOT_COUNT:
        *out_mr0 = (seL4_Word)s_next_slot;
        reply_len = 1;
        break;
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
        reply_len = 1;
        break;
    }
    case TM_REQ_PING_CLIENTINFO: {
        /* Demo: exercise ConnectClientInfo from inside the dispatch
         * loop.  The badge IS the scoid of the calling connection. */
        struct _client_info ci;
        int rc = ConnectClientInfo((int)badge, &ci, 0);
        *out_mr0 = mr0 + 1;
        *out_mr1 = (rc == 0) ? (seL4_Word)ci.pid : (seL4_Word)-1;
        reply_len = 2;
        break;
    }

    /* ---------- procmgr ---------- */
    case TM_REQ_CHANNEL_CREATE: {
        int chid = (int)mr0;
        unsigned flags = (unsigned)mr1;
        unsigned long recv_slot = 0;
        int rc = tm_channel_create(caller, chid, flags, &recv_slot);
        if (rc) { err = (seL4_Word)(-rc); }
        else    { *out_mr0 = recv_slot; reply_len = 1; }
        break;
    }
    case TM_REQ_CHANNEL_DESTROY: {
        int rc = tm_channel_destroy(caller, (seL4_CPtr)mr0);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_CONNECT_ATTACH: {
        unsigned long send_slot = 0;
        int rc = tm_connect_attach(caller, (pid_t)mr0, (int)mr1,
                                    (unsigned)mr2, &send_slot);
        if (rc) { err = (seL4_Word)(-rc); }
        else    { *out_mr0 = send_slot; reply_len = 1; }
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
        seL4_CPtr tcb_slot = 0, ntfn_slot = 0;
        unsigned prio_byte = (unsigned)(mr3 & 0xffu);
        unsigned affinity  = (unsigned)((mr3 >> 8) & 0xffu);
        int rc = tm_thread_alloc(caller,
                                  (unsigned long)mr0, (unsigned)mr1,
                                  (unsigned long)mr2,
                                  prio_byte, affinity,
                                  &new_tid, &tcb_slot, &ntfn_slot);
        if (rc) { err = (seL4_Word)(-rc); }
        else {
            *out_mr0 = (seL4_Word)tcb_slot;
            *out_mr1 = (seL4_Word)ntfn_slot;
            *out_mr2 = (seL4_Word)new_tid;
            reply_len = 3;
        }
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
        *out_mr0 = (seL4_Word)target;
        *out_mr1 = (seL4_Word)proc->signal_chid;
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
        *out_mr0 = (seL4_Word)len;
        /* Bytes ride in msg[4..]; framing covers the MR0..3 quad
         * plus enough words for the path payload. */
        reply_len = 4 + (len + 7) / 8;
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
        /* MR0 = total nanoseconds.  Block via SaveCaller until the
         * timer sweep wakes us. */
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
        tm_cred_t cred;
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

    /* ---------- pathmgr / IO ---------- */
    case TM_REQ_OPEN: {
        seL4_CPtr slot = 0;
        int rc = tm_io_open(caller, (unsigned)mr0, &slot);
        if (rc) { err = (seL4_Word)(-rc); break; }
        *out_mr0 = slot;
        reply_len = 1;
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
        *out_mr0 = (seL4_Word)bytes;
        reply_len = 4 + (bytes + 7) / 8;
        break;
    }
    case TM_REQ_LSEEK: {
        /* MR0 = whence, MR1 = signed 64-bit offset. */
        long off = 0;
        int rc = tm_lseek(caller, badge, (int)mr0, (long)mr1, &off);
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

static unsigned cstrlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

static void print_banner(void)
{
    static const char *prefix = "QSOE: Quick & Secure Operating Environment ";
    unsigned inner = 1 + cstrlen(prefix) + cstrlen(QSOE_VSHORT) + 1;

    sel4_debug_putchar('\f');
    sel4_debug_putchar('+');
    for (unsigned i = 0; i < inner; ++i) sel4_debug_putchar('-');
    sel4_debug_puts("+\n");

    sel4_debug_puts("| ");
    sel4_debug_puts(prefix);
    sel4_debug_puts(QSOE_VSHORT);
    sel4_debug_puts(" |\n");

    sel4_debug_putchar('+');
    for (unsigned i = 0; i < inner; ++i) sel4_debug_putchar('-');
    sel4_debug_puts("+\n\n");
}

static seL4_CPtr find_largest_ram_untyped(seL4_BootInfo *bi)
{
    unsigned n = bi->untyped.end - bi->untyped.start;
    unsigned best = (unsigned)-1;
    unsigned best_bits = 0;
    for (unsigned i = 0; i < n; ++i) {
        if (bi->untypedList[i].isDevice) continue;
        if (bi->untypedList[i].sizeBits > best_bits) {
            best_bits = bi->untypedList[i].sizeBits;
            best = i;
        }
    }
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
    print_banner();
    qsoe_libqsoe_init(bi->ipcBuffer, QSOE_PID_TASKMAN);

    /* Parse FDT (if seL4 published one) and build the syscfg blob.
     * If anything fails, syscfg-dependent handlers fall back to the
     * compile-time hardcoded values — boot still completes. */
    unsigned dtb_size = 0;
    const void *dtb = find_fdt_in_extra_bi(bi, &dtb_size);
    if (dtb && tm_syscfg_build(dtb) == 0) {
        sel4_debug_puts("taskman: syscfg built from FDT\n");
    } else {
        sel4_debug_puts("taskman: no FDT in extra-BI; syscfg falls back\n");
    }

    /* Pick up timebase-Hz from syscfg if available, hardcode otherwise. */
    {
        uint64_t hz = 0;
        if (tm_syscfg_find_u64(TM_SYSCFG_TAG_TIMEBASE_HZ, &hz) == 0 && hz) {
            qsoe_time_freq_hz = hz;
        } else {
            qsoe_time_freq_hz = TM_CLOCK_FREQ_HZ;
        }
    }

    seL4_CPtr ut = find_largest_ram_untyped(bi);
    if (ut == 0) {
        sel4_debug_puts("FATAL: no RAM untyped\n");
        for (;;) __asm__ volatile("nop");
    }

    seL4_CPtr uart_ut = find_device_untyped_for_paddr(bi, 0x10000000UL);
    tm_set_uart_untyped(uart_ut);
    tm_mem_set_bootinfo(bi);  /* needed by MAP_PHYS to walk device-UTs */
    if (uart_ut == 0) {
        sel4_debug_puts("warn: no device-untyped at 0x10000000\n");
    }

    tm_init(ut, seL4_CapInitThreadCNode, bi->empty.start);

    seL4_CPtr primary_ep = bi->empty.start;
    s_next_slot = bi->empty.start + 1;
    seL4_Word rerr = qsoe_untyped_retype(ut, seL4_EndpointObject, 0,
                                          seL4_CapInitThreadCNode, 0, 0,
                                          primary_ep, 1);
    if (rerr != 0) {
        sel4_debug_puts("FATAL: failed to retype primary endpoint\n");
        for (;;) __asm__ volatile("nop");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, 1,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register primary channel\n");
        for (;;) __asm__ volatile("nop");
    }

    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_CONSOLE_CHID,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register console channel\n");
        for (;;) __asm__ volatile("nop");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_CPIOFS_CHID,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register cpiofs channel\n");
        for (;;) __asm__ volatile("nop");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_DEVNULL_CHID,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register /dev/null channel\n");
        for (;;) __asm__ volatile("nop");
    }
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_DEVZERO_CHID,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register /dev/zero channel\n");
        for (;;) __asm__ volatile("nop");
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
            sel4_debug_puts("FATAL: pathmgr register /dev/console failed\n");
            for (;;) __asm__ volatile("nop");
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
            sel4_debug_puts("FATAL: pathmgr register / failed\n");
            for (;;) __asm__ volatile("nop");
        }
    }

    /* /dev/tty — POSIX controlling-terminal alias.  Symlink to
     * /dev/console so repath()s on the latter (e.g. devc-ser8250
     * coming up at boot) carry over.  isatty(open("/dev/tty")) is
     * how qsh detects its tty — once this resolves, qsh's edit.c
     * editor (with arrow-key history) takes over. */
    if (tm_pathmgr_symlink("/dev/tty", "/dev/console") != 0) {
        sel4_debug_puts("FATAL: pathmgr symlink /dev/tty failed\n");
        for (;;) __asm__ volatile("nop");
    }
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
            sel4_debug_puts("FATAL: pathmgr register /dev/null failed\n");
            for (;;) __asm__ volatile("nop");
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
            sel4_debug_puts("FATAL: pathmgr register /dev/zero failed\n");
            for (;;) __asm__ volatile("nop");
        }
    }

    unsigned long cpio_len = (unsigned long)
        (_userland_cpio_end - _userland_cpio_start);
    tm_set_userland_cpio(_userland_cpio_start, cpio_len);
    tm_set_primary_ep(primary_ep);
    tm_cpiofs_set_cpio(_userland_cpio_start, cpio_len);

    unsigned long elf_size = 0;
    const void *elf = cpio_get_file(_userland_cpio_start, cpio_len,
                                     "sbin/init", &elf_size);
    if (!elf) {
        sel4_debug_puts("FATAL: sbin/init not found in CPIO\n");
        for (;;) __asm__ volatile("nop");
    }
    pid_t init_pid = tm_pid_alloc();
    if (!init_pid) {
        sel4_debug_puts("FATAL: pid allocator empty\n");
        for (;;) __asm__ volatile("nop");
    }
    sel4_debug_puts("taskman: spawning /sbin/init (pid=");
    {
        char d = '0' + (char)(init_pid & 0x7);
        sel4_debug_putchar(d);
    }
    sel4_debug_puts(")...\n");
    static const char *boot_argv0 = "init";
    const char *boot_argv[1] = { boot_argv0 };
    int sr = tm_spawn(elf, elf_size, init_pid, primary_ep,
                       /*argc=*/1, boot_argv,
                       /*envc=*/0, 0,
                       /*elf_name=*/"sbin/init");
    if (sr != 0) {
        sel4_debug_puts("FATAL: tm_spawn returned non-zero\n");
        for (;;) __asm__ volatile("nop");
    }
    sel4_debug_puts("taskman: dispatcher ready\n");

    seL4_Word badge;
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t info = qsoe_sys_recv(primary_ep, &badge,
                                             &mr0, &mr1, &mr2, &mr3);
    for (;;) {
        seL4_Word r0, r1, r2, r3;
        int no_reply = 0;
        seL4_MessageInfo_t reply_info = tm_dispatch(info, badge,
                                                     mr0, mr1, mr2, mr3,
                                                     &r0, &r1, &r2, &r3,
                                                     &no_reply);
        if (no_reply) {
            mr0 = 0; mr1 = 0; mr2 = 0; mr3 = 0;
            info = qsoe_sys_recv(primary_ep, &badge,
                                  &mr0, &mr1, &mr2, &mr3);
        } else {
            mr0 = r0; mr1 = r1; mr2 = r2; mr3 = r3;
            info = qsoe_sys_reply_recv(primary_ep, reply_info, &badge,
                                        &mr0, &mr1, &mr2, &mr3);
        }
    }
    return 0;
}
