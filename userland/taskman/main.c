/*
 * taskman — central system server (QSOE).
 *
 * v0.2: wires up the four QNX-style IPC lifecycle calls
 * (ChannelCreate / ChannelDestroy / ConnectAttach / ConnectDetach)
 * via libqsoe and exercises them in a self-test. The dispatch loop
 * for serving these calls from *other* processes lands in v0.3, when
 * we have a second process to call from.
 */

#include "sel4_syscalls.h"
#include "sel4_types.h"
#include "qsoe_invoke.h"
#include "server.h"
#include "spawn.h"
#include "pathmgr.h"
#include "console.h"
#include "cpiofs.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include "../libqsoe/include/qsoe/slots.h"
#include "../libqsoe/include/qsoe/wire.h"
#include <qsoe/sys_version.h>
#include <cpio/cpio.h>

extern char _userland_cpio_start[];
extern char _userland_cpio_end[];

/* Dispatch one incoming message. Inputs: the request's badge + MRs.
 * Outputs: the reply tag and the reply MRs (up to 4 — most calls only
 * fill 0 or 1; the introspection calls fill up to 3). */
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

    switch (label) {
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
        seL4_CPtr recv_slot = (seL4_CPtr)mr0;
        int rc = tm_channel_destroy(caller, recv_slot);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_CONNECT_ATTACH: {
        pid_t target_pid = (pid_t)mr0;
        int target_chid = (int)mr1;
        unsigned flags = (unsigned)mr2;
        unsigned long send_slot = 0;
        int rc = tm_connect_attach(caller, target_pid, target_chid,
                                    flags, &send_slot);
        if (rc) { err = (seL4_Word)(-rc); }
        else    { *out_mr0 = send_slot; reply_len = 1; }
        break;
    }
    case TM_REQ_CONNECT_DETACH: {
        seL4_CPtr send_slot = (seL4_CPtr)mr0;
        int rc = tm_connect_detach(caller, send_slot);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_CONNECT_SERVER_INFO: {
        pid_t server_pid = 0;
        int server_chid = 0;
        seL4_Word scoid = 0;
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
        pid_t client_pid = 0, sid = 0;
        unsigned cflags = 0;
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
    case TM_REQ_PROCESS_CREATE: {
        /* v0.4.4 wire layout:
         *   MR0 = argc       MR1 = envc
         *   MR2 = path_len   MR3 = total_strs_bytes (sum over all strings)
         *   ipcbuf->msg[4..] = path bytes + argv strings + envp strings,
         *     each string NUL-terminated.
         *
         * The kernel only transfers msg[4..length-1]; MR0..3 are
         * register-passed. Strings are unpacked into a static staging
         * area below, scanned for NULs to build argv[]/envp[] pointer
         * arrays, and handed to tm_process_create_by_name. */
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
        /* Copy strings out of the IPC buffer into the staging area. */
        const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
        for (unsigned i = 0; i < total_strs; ++i) s_staging[i] = (char)src[i];

        /* Path comes first; argv strings next; envp strings last. */
        const char *path = s_staging;
        unsigned off = plen + 1;  /* one extra byte for the NUL after path */
        for (int i = 0; i < argc; ++i) {
            if (off >= total_strs) { err = (seL4_Word)EINVAL; break; }
            s_argv[i] = &s_staging[off];
            while (off < total_strs && s_staging[off] != 0) ++off;
            ++off;  /* skip the NUL */
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
        else    { *out_mr0 = (seL4_Word)new_pid; reply_len = 1; }
        break;
    }
    case TM_REQ_DEBUG_SLOT_COUNT: {
        /* Diagnostic: return taskman's bump-pointer for cap-leak tests. */
        extern seL4_CPtr s_next_slot;
        *out_mr0 = (seL4_Word)s_next_slot;
        reply_len = 1;
        break;
    }
    case TM_REQ_PULSE_SEND: {
        /* MR0 = connection slot (in caller's CSpace), MR1 = priority,
         * MR2 = code (signed), MR3 = value. */
        int rc = tm_pulse_send(caller, (seL4_CPtr)mr0,
                                (int)mr1, (int)(int8_t)mr2, (int)mr3);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_PULSE_FETCH: {
        /* MR0 = recv_slot. Reply MR0=code, MR1=value, MR2=sender_pid,
         * MR3=scoid. label=0 success, ENOENT empty. */
        tm_pulse_t p;
        int scoid = 0;
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
    case TM_REQ_PROCESS_TERMINATE: {
        /* MR0 = target pid (0 = self), MR1 = exit status.
         * For self-terminate, the caller's TCB is revoked inside the
         * handler — we signal the dispatch loop to skip Reply. */
        pid_t target = (pid_t)mr0;
        if (target == 0) target = caller;
        int rc = tm_process_terminate(target, (int)mr1);
        if (rc) { err = (seL4_Word)(-rc); }
        else if (target == caller) {
            /* Caller's TCB is gone — there's no thread to reply to. */
            *out_no_reply = 1;
        }
        break;
    }
    case TM_REQ_THREAD_ALLOC: {
        int new_tid = 0;
        seL4_CPtr tcb_slot = 0, ntfn_slot = 0;
        /* MR3 packs prio (low 8 bits) + affinity (next 8 bits). */
        unsigned prio_byte    = (unsigned)(mr3 & 0xffu);
        unsigned affinity     = (unsigned)((mr3 >> 8) & 0xffu);
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
    case TM_REQ_OPEN: {
        /* MR0 = path length in bytes. Path bytes ride in msg[4..]. */
        unsigned plen = (unsigned)mr0;
        if (plen == 0 || plen >= 128) { err = (seL4_Word)EINVAL; break; }

        /* Copy + NUL-terminate the path locally so resolve() sees a
         * proper C string. */
        static char s_open_path[128];
        const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
        for (unsigned i = 0; i < plen; ++i) s_open_path[i] = (char)src[i];
        s_open_path[plen] = 0;

        tm_pathmgr_obj_t obj;
        unsigned consumed = 0;
        int rc = tm_pathmgr_resolve(s_open_path, &obj, &consumed);
        if (rc) { err = (seL4_Word)(-rc); break; }

        /* ConnectAttach mints a badged Send cap on
         * (server_pid, server_chid) into the caller's CSpace.
         * Same code path for in-taskman and external resmgrs. */
        seL4_CPtr slot = 0;
        rc = tm_connect_attach(caller, obj.server_pid, obj.server_chid,
                                0, &slot);
        if (rc) { err = (seL4_Word)(-rc); break; }

        /* v0.6.0: handlers that need per-fd state populate it here.
         * cpiofs stores the (data, size) of the resolved file so
         * subsequent IO_READ can resume from the right offset. */
        if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CPIOFS) {
            seL4_Word badge = 0;
            if (tm_connection_badge_by_slot(caller, slot, &badge) == 0) {
                int orc = tm_cpiofs_open(s_open_path, consumed, badge);
                if (orc) {
                    /* Roll back the mint — file not found. */
                    tm_connect_detach(caller, slot);
                    err = (seL4_Word)(-orc);
                    break;
                }
            }
        }
        *out_mr0 = slot;
        reply_len = 1;
        break;
    }
    case TM_REQ_CLOSE: {
        /* MR0 = the connection slot in the caller's CSpace. */
        seL4_CPtr slot = (seL4_CPtr)mr0;
        int rc = tm_connect_detach(caller, slot);
        if (rc) err = (seL4_Word)(-rc);
        break;
    }
    case TM_REQ_IO_WRITE: {
        /* Badge identifies the connection; we look up which channel
         * it points at and route. MR0 = nbytes (the count of bytes
         * the client placed into msg[4..]). */
        unsigned nbytes = (unsigned)mr0;
        pid_t srv_pid = 0;
        int srv_chid = 0;
        if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) {
            err = (seL4_Word)EBADF;
            break;
        }
        if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CONSOLE_CHID) {
            unsigned wrote = tm_console_write(nbytes);
            *out_mr0 = (seL4_Word)wrote;
            reply_len = 1;
        } else if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
            err = (seL4_Word)EROFS;  /* cpiofs is read-only */
        } else {
            err = (seL4_Word)ENOSYS;  /* external resmgr — v0.6+ */
        }
        break;
    }
    case TM_REQ_IO_READ: {
        unsigned want = (unsigned)mr0;
        pid_t srv_pid = 0;
        int srv_chid = 0;
        if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) {
            err = (seL4_Word)EBADF;
            break;
        }
        if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CONSOLE_CHID) {
            unsigned got = 0;
            int rc = tm_console_read(want, &got);
            if (rc) { err = (seL4_Word)(-rc); break; }
            *out_mr0 = (seL4_Word)got;
            reply_len = 1;
        } else if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
            unsigned got = 0;
            int rc = tm_cpiofs_read(badge, want, &got);
            if (rc) { err = (seL4_Word)(-rc); break; }
            *out_mr0 = (seL4_Word)got;
            /* The byte payload lives in msg[4..]; the kernel only
             * transfers msg[4..length-1] across IPC, so the reply
             * length needs to cover those words (the +4 accounts for
             * the four register-passed MRs). */
            reply_len = 4 + (got + 7) / 8;
        } else {
            err = (seL4_Word)ENOSYS;
        }
        break;
    }
    case TM_REQ_PING_CLIENTINFO: {
        /* Demo: exercise ConnectClientInfo from inside the dispatch
         * loop. The badge attached to this incoming message IS the
         * scoid of the calling connection. We return the client's
         * pid in MR1 alongside the usual +1 echo in MR0. */
        struct _client_info ci;
        int rc = ConnectClientInfo((int)badge, &ci, 0);
        *out_mr0 = mr0 + 1;
        *out_mr1 = (rc == 0) ? (seL4_Word)ci.pid : (seL4_Word)-1;
        reply_len = 2;
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
    /* Clear-screen via Ctrl-L, then a bordered banner sized to fit
     * "QSOE: Quick & Secure Operating Environment <version>". */
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

int main(seL4_BootInfo *bi)
{
    print_banner();
    qsoe_libqsoe_init(bi->ipcBuffer, QSOE_PID_TASKMAN);

    seL4_CPtr ut = find_largest_ram_untyped(bi);
    if (ut == 0) {
        sel4_debug_puts("FATAL: no RAM untyped\n");
        for (;;) __asm__ volatile("nop");
    }

    tm_init(ut, seL4_CapInitThreadCNode, bi->empty.start);

    /* v0.3.0: spawn tester. We don't yet have taskman's primary
     * endpoint allocated by ChannelCreate — for v0.3.0 we create it
     * the same way we will in v0.3.2, by hand, so that spawn() can
     * mint a Send cap to it for tester's CSpace slot 1. */
    seL4_CPtr primary_ep = bi->empty.start; /* very next free slot */
    /* Bump tm_init's allocator past it so spawn() doesn't reuse it. */
    extern seL4_CPtr s_next_slot;
    s_next_slot = bi->empty.start + 1;
    seL4_Word rerr = qsoe_untyped_retype(ut, seL4_EndpointObject, 0,
                                          seL4_CapInitThreadCNode, 0, 0,
                                          primary_ep, 1);
    if (rerr != 0) {
        sel4_debug_puts("FATAL: failed to retype primary endpoint\n");
        for (;;) __asm__ volatile("nop");
    }
    /* Register taskman's primary channel in the registry so ConnectAttach
     * from other processes can find it as (pid=1, chid=1). The master
     * and recv cap are the same slot — taskman invokes the cap directly
     * for both seL4_Recv (server-side) and as the mint source for new
     * connections. */
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, 1,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register primary channel\n");
        for (;;) __asm__ volatile("nop");
    }

    /* v0.5.0: register the console channel (TM_CONSOLE_CHID, shares
     * primary_ep so the dispatch loop receives all traffic on one
     * Recv; we route by badge -> connection -> channel). Then bring
     * up the path manager and register /dev/console pointing at the
     * in-taskman console handler. */
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_CONSOLE_CHID,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register console channel\n");
        for (;;) __asm__ volatile("nop");
    }
    /* v0.6.0: register the cpiofs channel (also shares primary_ep). */
    if (tm_channel_register_existing(QSOE_PID_TASKMAN, TM_CPIOFS_CHID,
                                      primary_ep, primary_ep) != 0) {
        sel4_debug_puts("FATAL: failed to register cpiofs channel\n");
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
    /* v0.6.0: register cpiofs at "/". Longest-prefix-match means
     * /dev/console and (future) /dev/ser1 still resolve to their
     * specific handlers; everything else under / goes to cpiofs. */
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

    /* v0.4.1: hand the embedded CPIO and primary endpoint to server.c
     * so TM_REQ_PROCESS_CREATE handlers can locate ELFs and badge
     * SYSMGR caps. */
    unsigned long cpio_len = (unsigned long)
        (_userland_cpio_end - _userland_cpio_start);
    tm_set_userland_cpio(_userland_cpio_start, cpio_len);
    tm_set_primary_ep(primary_ep);
    tm_cpiofs_set_cpio(_userland_cpio_start, cpio_len);

    /* Find tester.elf in the embedded userland CPIO. v0.6.0 moved
     * entries under bin/, in line with cpiofs's expected layout. */
    unsigned long elf_size = 0;
    const void *elf = cpio_get_file(_userland_cpio_start, cpio_len,
                                     "bin/tester.elf", &elf_size);
    if (!elf) {
        sel4_debug_puts("FATAL: bin/tester.elf not found in CPIO\n");
        for (;;) __asm__ volatile("nop");
    }
    pid_t tester_pid = tm_pid_alloc();
    if (!tester_pid) {
        sel4_debug_puts("FATAL: pid allocator empty\n");
        for (;;) __asm__ volatile("nop");
    }
    sel4_debug_puts("taskman: spawning tester (pid=");
    {
        char d = '0' + (char)(tester_pid & 0x7);
        sel4_debug_putchar(d);
    }
    sel4_debug_puts(")...\n");
    /* Boot-time spawn of tester: no argv, no envp. */
    static const char *boot_argv0 = "tester";
    const char *boot_argv[1] = { boot_argv0 };
    int sr = tm_spawn(elf, elf_size, tester_pid, primary_ep,
                       /*argc=*/1, boot_argv,
                       /*envc=*/0, 0);
    if (sr != 0) {
        sel4_debug_puts("FATAL: tm_spawn returned non-zero\n");
        for (;;) __asm__ volatile("nop");
    }
    sel4_debug_puts("taskman: dispatcher ready\n");

    /* Dispatch loop: ReplyRecv pattern. seL4_Recv blocks until the
     * first message; thereafter ReplyRecv atomically sends the reply
     * to the previous caller and waits for the next. */
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
            /* Caller's TCB was revoked (e.g. self-terminate). Skip the
             * reply phase and just receive the next request. */
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
