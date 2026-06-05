/*
 * libqsoe/src/msg.c — MsgSend / MsgReceive / MsgReply.
 *
 * These are the QNX-style synchronous IPC primitives. Unlike the four
 * lifecycle calls (ChannelCreate etc.) which always go through taskman,
 * Msg{Send,Receive,Reply} operate directly on the user's channel/
 * connection capabilities — peer-to-peer between server and client,
 * with taskman uninvolved.
 *
 * On non-MCS seL4 the reply capability is per-thread (tcbCaller slot),
 * not per-message, so MsgReply takes the rcvid only for QNX API
 * compatibility — internally it's the implicit reply cap. See <qsoe-system.h>
 * for the caveat.
 *
 * Byte ↔ word marshalling: we treat the IPC buffer's msg[] array as
 * a flat byte buffer. msg[0..3] is reserved for the kernel's
 * "fast-path" register slots (a2-a5) — on send we read from msg[0..3]
 * after packing, on receive we copy the in-register reply MRs back
 * into msg[0..3] so unpack_bytes can find them.
 */

#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <stddef.h>
#include "state.h"
#include "libc.h"           /* musl-style struct __libc + the `libc` alias */

/* Pull in just the types and ecall wrappers we need from taskman's
 * tree. These will move into a libqsoe-internal header once we split
 * the syscall stubs from taskman. */
#include "sel4_types.h"
#include "qsoe_invoke.h"


/* The IPC buffer pointer lives in the current thread's qsoe_tcb_t.
 * For the main thread of every process that's qsoe_main_tcb, which
 * the crt0 already pointed `tp` at before this runs. */

/* The libqsoe init hook gets called from each process's crt0 / first
 * libqsoe call. Idempotent.
 *
 *   buf       — IPC buffer the kernel mapped for this thread.
 *   self_pid  — what spawn.c passed in a0 (taskman: QSOE_PID_TASKMAN).
 *
 * For non-taskman processes this also pre-binds TASKMAN_COID to
 * QSOE_CAP_TASKMAN_EP. spawn.c minted the cap into that CSpace slot at
 * spawn time, so the connection is already live — we just teach
 * libqsoe's coid table about it. */
/* Minimum aux-vector mallocng's init walks via __libc.auxv -- just
 * AT_PAGESZ followed by AT_NULL.  Mirrors NQ's qsoe_min_auxv (see
 * nq/libc/libc_init.c) so behaviour is identical across kernels. */
static const size_t qsoe_min_auxv[] = {
    6,    4096,   /* AT_PAGESZ = page size */
    0,    0,      /* AT_NULL */
};

void qsoe_libc_init(void *ipcbuf, pid_t self_pid)
{
    /* crt0 stashes the kernel-set a0 (= taskman's spawn-time pid)
     * into s3 and passes it here as self_pid; ipcbuf is wired as
     * NULL today (taskman maps the IPC frame at the fixed VA
     * 0x1FE000 for every child, so we substitute that here). */
    if (!ipcbuf) ipcbuf = (void *)0x1FE000UL;

    qsoe_curthr()->ipcbuf   = ipcbuf;
    qsoe_curthr()->self_pid = self_pid;
    if (self_pid != QSOE_PID_TASKMAN) {
        qsoe_state_bind_coid(TASKMAN_COID, QSOE_CAP_TASKMAN_EP);
    }

    /* musl's malloc + several other libc paths read libc.auxv and
     * libc.page_size.  __init_libc would normally fill these from a
     * kernel-supplied aux-vector; rtld isn't on the LQ dyn-link path
     * (taskman pre-relocates), so patch the bare minimum here. */
    libc.auxv      = (size_t *)(uintptr_t)qsoe_min_auxv;
    libc.page_size = 4096;

    /* Pre-bind stdio fds 0/1/2 to the stdio caps taskman minted into
     * our CSpace at spawn (see spawn.c step 5c).  After this,
     * write(1, ...) / write(2, ...) "just work" -- same posture as
     * POSIX fd inheritance across exec. */
    if (self_pid != QSOE_PID_TASKMAN) {
        qsoe_state_force_bind_coid(0, QSOE_CAP_STDIN_CONNECT);
        qsoe_state_force_bind_coid(1, QSOE_CAP_STDOUT_CONNECT);
        qsoe_state_force_bind_coid(2, QSOE_CAP_STDERR_CONNECT);
    }
}

/* IPC-buffer byte capacity: 120 words × 8 bytes. */
#define QSOE_MSG_MAX_BYTES (seL4_MsgMaxLength * 8)

/* v0.4 deferred cancellation point. Each libqsoe IPC entry checks
 * the current thread's cancel_pending flag and, if set, terminates
 * the thread with status (void *)-1 — QNX's PTHREAD_CANCELED. */
static inline void qsoe_cancel_point(void)
{
    if (qsoe_curthr()->cancel_pending) {
        ThreadDestroy(0, 0, (void *)(unsigned long)-1L);
    }
}

/* Pack `nbytes` from `src` into the IPC buffer's msg[] view. Returns
 * the number of seL4 words occupied (= ceil(nbytes/8)).
 *
 * `src == NULL` is the QRV-style "caller pre-packed the IPC buffer"
 * idiom -- the pathmgr client and several other tm_call_* helpers stage
 * msg[0..N] directly, then call MsgSend with smsg=NULL so this routine
 * leaves the buffer untouched and only computes the word count.  Used
 * to be a silent NULL-deref crash; per the no-silent-stubs rule the
 * fast-path now explicitly honours the contract. */
static unsigned pack_bytes(const void *src, unsigned nbytes)
{
    if (nbytes > QSOE_MSG_MAX_BYTES) nbytes = QSOE_MSG_MAX_BYTES;
    unsigned nwords = (nbytes + 7) / 8;
    if (!src) return nwords;
    /* Zero the last partial word so unused tail bytes don't leak the
     * previous message's contents to the receiver. */
    if (nwords > 0) qsoe_ipcbuf->msg[nwords - 1] = 0;
    unsigned char *dst = (unsigned char *)qsoe_ipcbuf->msg;
    const unsigned char *s = src;
    for (unsigned i = 0; i < nbytes; ++i) dst[i] = s[i];
    return nwords;
}

/* Unpack up to `want` bytes from the IPC buffer's msg[] view into
 * `dst`. `avail` is the byte count the sender provided (we read at
 * most this many; the rest is junk). */
static void unpack_bytes(void *dst, unsigned avail, unsigned want)
{
    unsigned n = avail < want ? avail : want;
    if (n > QSOE_MSG_MAX_BYTES) n = QSOE_MSG_MAX_BYTES;
    const unsigned char *s = (const unsigned char *)qsoe_ipcbuf->msg;
    unsigned char *d = dst;
    for (unsigned i = 0; i < n; ++i) d[i] = s[i];
}

int MsgSend(int coid, const void *smsg, int sbytes,
            void *rmsg, int rbytes)
{
    qsoe_cancel_point();
    if (sbytes < 0 || rbytes < 0) { qsoe_errno = EINVAL; return -1; }
    seL4_CPtr send = qsoe_state_coid_to_slot(coid);
    if (!send) { qsoe_errno = EBADF; return -1; }

    unsigned nwords = pack_bytes(smsg, (unsigned)sbytes);
    seL4_Word mr0 = qsoe_ipcbuf->msg[0];
    seL4_Word mr1 = qsoe_ipcbuf->msg[1];
    seL4_Word mr2 = qsoe_ipcbuf->msg[2];
    seL4_Word mr3 = qsoe_ipcbuf->msg[3];

    /* QRV-style callers (procmgr_detach, tm_call_path, the pathmgr
     * client) stage their TM_REQ_* opcode in `qsoe_ipcbuf->tag` and
     * call MsgSend(coid, NULL, sbytes, 0, 0).  That opcode rides
     * through here as the seL4 MessageInfo label; without this, the
     * label arrives at taskman as 0 and the dispatcher's default
     * branch silently echoes "success", masking the no-op.  Symptoms
     * before this fix: pathmgr_register/procmgr_detach return 0 but
     * taskman never sees the request -- daemons hang waiting on a
     * waitpid reply that nothing will ever deliver. */
    unsigned send_label = (unsigned)qsoe_ipcbuf->tag;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(send_label, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(send, tag, &mr0, &mr1, &mr2, &mr3);

    /* Stage reply MRs back into msg[0..3] for unpack_bytes, and the
     * reply's label into msg.tag so QRV-style pre-packed callers (the
     * pathmgr client, tm_call_*) can read errno without re-decoding the
     * MessageInfo themselves. */
    qsoe_ipcbuf->tag    = (unsigned long)seL4_MessageInfo_get_label(reply);
    qsoe_ipcbuf->msg[0] = mr0;
    qsoe_ipcbuf->msg[1] = mr1;
    qsoe_ipcbuf->msg[2] = mr2;
    qsoe_ipcbuf->msg[3] = mr3;

    unsigned reply_bytes = (unsigned)seL4_MessageInfo_get_length(reply) * 8;
    if (rmsg) unpack_bytes(rmsg, reply_bytes, (unsigned)rbytes);
    return 0;
}

int MsgReceive(int chid, void *msg, int bytes, struct _msg_info *info)
{
    qsoe_cancel_point();
    if (bytes < 0) { qsoe_errno = EINVAL; return -1; }
    seL4_CPtr recv = qsoe_state_chid_to_slot(chid);
    if (!recv) { qsoe_errno = EBADF; return -1; }

    seL4_Word badge;
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = qsoe_sys_recv(recv, &badge,
                                            &mr0, &mr1, &mr2, &mr3);

    /* v0.4.3: bound-Notification pulse wake. If the receive resolved
     * via the bound Notification (badge has QSOE_NTFN_BADGE_BIT set
     * because taskman minted the Signal-cap with that badge), fetch
     * the queued pulse from taskman and return it as a _pulse rather
     * than treating MRs as a regular message. The high bit can't
     * collide with EP-message badges, which encode pids (≤ 255). */
    if (badge & QSOE_NTFN_BADGE_BIT) {
        seL4_Word p_mr0 = (seL4_Word)recv;
        seL4_Word p_mr1 = 0, p_mr2 = 0, p_mr3 = 0;
        seL4_MessageInfo_t p_tag = seL4_MessageInfo_new(TM_REQ_PULSE_FETCH,
                                                         0, 0, 1);
        seL4_MessageInfo_t p_reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, p_tag,
                                                    &p_mr0, &p_mr1, &p_mr2, &p_mr3);
        seL4_Word p_err = seL4_MessageInfo_get_label(p_reply);
        if (p_err == 0) {
            int8_t  code   = (int8_t)p_mr0;
            int32_t val    = (int32_t)p_mr1;
            pid_t   sender = (pid_t)p_mr2;
            int     scoid  = (int)p_mr3;
            if (msg && bytes >= (int)sizeof(struct _pulse)) {
                struct _pulse *p = (struct _pulse *)msg;
                p->type    = _PULSE_TYPE;
                p->subtype = 0;
                p->code    = code;
                p->reserved[0] = p->reserved[1] = p->reserved[2] = 0;
                p->value.sival_int = val;
                p->scoid   = scoid;
            }
            if (info) {
                info->nd        = ND_LOCAL_NODE;
                info->pid       = sender;
                info->chid      = chid;
                info->scoid     = scoid;
                info->coid      = 0;
                info->msglen    = (int)sizeof(struct _pulse);
                info->srcmsglen = (int)sizeof(struct _pulse);
                info->dstmsglen = bytes;
                info->priority  = 0;
                info->flags     = QSOE_MI_PULSE;
                info->label     = 0;
            }
            return 0;
        }
        /* Notification fired but no pulse queued — race with another
         * fetcher. Fall through and treat as if the wake-up was
         * spurious; MRs are zero so callers see an empty EP message. */
    }

    qsoe_ipcbuf->msg[0] = mr0;
    qsoe_ipcbuf->msg[1] = mr1;
    qsoe_ipcbuf->msg[2] = mr2;
    qsoe_ipcbuf->msg[3] = mr3;

    unsigned in_bytes = (unsigned)seL4_MessageInfo_get_length(tag) * 8;
    if (msg) unpack_bytes(msg, in_bytes, (unsigned)bytes);

    if (info) {
        info->nd        = ND_LOCAL_NODE;
        info->pid       = (pid_t)badge;   /* badge == sender pid in QSOE */
        info->chid      = chid;
        info->scoid     = (int)badge;
        info->coid      = 0;              /* not knowable server-side */
        info->msglen    = (int)in_bytes;
        info->srcmsglen = (int)in_bytes;
        info->dstmsglen = bytes;
        info->priority  = 0;
        info->flags     = 0;
        info->label     = (unsigned)seL4_MessageInfo_get_label(tag);
    }

    /* rcvid: in QNX it's a token identifying this specific receive.
     * On non-MCS seL4 the reply cap is implicit (one per server
     * thread), so the rcvid is purely for the client→server protocol's
     * benefit. We return the badge: it identifies the sender, and the
     * MsgReply that consumes the rcvid uses the implicit reply cap. */
    return (int)badge;
}

int MsgSavereply(int rcvid)
{
    /* Already saved — caller is double-saving; just hand the same
     * stable rcvid back. */
    if ((unsigned)rcvid & QSOE_RCVID_SAVED) return rcvid;

    unsigned long slot = qsoe_state_alloc_empty_slot();
    if (!slot) { qsoe_errno = EAGAIN; return -1; }
    seL4_Word err = qsoe_cnode_save_caller(QSOE_CAP_CNODE_SELF, slot,
                                            QSOE_CAP_CNODE_DEPTH);
    if (err) {
        qsoe_state_free_empty_slot(slot);
        qsoe_errno = (int)err;
        return -1;
    }
    return (int)(QSOE_RCVID_SAVED | (unsigned)slot);
}

int MsgReply(int rcvid, int status, const void *msg, int bytes)
{
    qsoe_cancel_point();
    if (bytes < 0) { qsoe_errno = EINVAL; return -1; }

    unsigned nwords = pack_bytes(msg, (unsigned)bytes);
    seL4_Word mr0 = qsoe_ipcbuf->msg[0];
    seL4_Word mr1 = qsoe_ipcbuf->msg[1];
    seL4_Word mr2 = qsoe_ipcbuf->msg[2];
    seL4_Word mr3 = qsoe_ipcbuf->msg[3];

    seL4_MessageInfo_t tag = seL4_MessageInfo_new((unsigned)status,
                                                   0, 0, nwords);
    if ((unsigned)rcvid & QSOE_RCVID_SAVED) {
        /* Deferred reply via SaveCaller'd slot: Send on the slot and
         * recycle it.  The kernel auto-clears the slot when the Send
         * consumes the reply cap on non-MCS. */
        unsigned long slot = (unsigned long)((unsigned)rcvid & ~QSOE_RCVID_SAVED);
        qsoe_sys_send((seL4_CPtr)slot, tag, mr0, mr1, mr2, mr3);
        qsoe_state_free_empty_slot(slot);
    } else {
        /* Normal reply via the implicit per-thread reply cap. */
        qsoe_sys_reply(tag, mr0, mr1, mr2, mr3);
    }
    return 0;
}

int MsgSendPulse(int coid, int priority, int code, int value)
{
    qsoe_cancel_point();
    seL4_CPtr send = qsoe_state_coid_to_slot(coid);
    if (!send) { qsoe_errno = EBADF; return -1; }

    /* MR0 = sender's coid slot, MR1 = priority, MR2 = code (8-bit
     * signed in low byte), MR3 = value. Taskman finds the target
     * channel via the connection registry keyed on (caller, slot). */
    seL4_Word mr0 = (seL4_Word)send;
    seL4_Word mr1 = (seL4_Word)priority;
    seL4_Word mr2 = (seL4_Word)((unsigned)code & 0xffu);
    seL4_Word mr3 = (seL4_Word)value;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PULSE_SEND,
                                                   0, 0, 4);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
}
