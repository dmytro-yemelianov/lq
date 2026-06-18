/*
 * libqsoe/src/msg.c — MsgSend / MsgReceive / MsgReply.
 *
 * These are the QNX-style synchronous IPC primitives. Unlike the four
 * lifecycle calls (ChannelCreate etc.) which always go through taskman,
 * Msg{Send,Receive,Reply} operate directly on the user's channel/
 * connection capabilities — peer-to-peer between server and client,
 * with taskman uninvolved.
 *
 * Under MCS the reply capability is a per-message reply object, not the
 * per-thread tcbCaller slot: MsgReply consumes this thread's active
 * reply object (or a stashed one for a deferred reply), and the rcvid is
 * the QNX-API token identifying the receive. See <sys/qsoe.h>.
 *
 * Byte ↔ word marshalling: we treat the IPC buffer's msg[] array as
 * a flat byte buffer. msg[0..3] is reserved for the kernel's
 * "fast-path" register slots (a2-a5) — on send we read from msg[0..3]
 * after packing, on receive we copy the in-register reply MRs back
 * into msg[0..3] so unpack_bytes can find them.
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include <qsoe/sigdeliver.h>   /* __qsoe_syschan_init (signal thread) */
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
    /* The main thread receives with the well-known reply object taskman
     * provisioned at spawn; worker threads carry their own (set in
     * ThreadCreate).  Reply objects cannot be shared between threads. */
    qsoe_curthr()->reply_cap = QSOE_CAP_REPLY;
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

        /* v0.7: cache the platform's rdtime frequency so ClockTime /
         * ClockCycles / nanosleep convert ticks->nsec without IPC.  One
         * round-trip per process at startup; the value is fixed for the
         * life of the system.  (Was lost when crt0 began calling main
         * directly; restored here on the live path.) */
        extern int qsoe_query_clock_freq(unsigned long *out_hz);
        extern unsigned long qsoe_time_freq_hz;
        (void)qsoe_query_clock_freq(&qsoe_time_freq_hz);

        /* Signals-as-pulses: bring up the per-process system thread +
         * signal channel before main runs (same shape as NQ).  Failure
         * is announced inside and non-fatal -- the process just cannot
         * receive signals. */
        __qsoe_syschan_init();
    }
}

/* IPC-buffer byte capacity: 120 words × 8 bytes. */
#define QSOE_MSG_MAX_BYTES (seL4_MsgMaxLength * 8)

/* ---- Bulk-IPC transport (doc/plans/bulk_ipc.txt) ----------------------
 *
 * seL4 IPC moves only the message registers (QSOE_MSG_MAX_BYTES).  A
 * MsgSend/MsgReply whose send OR reply exceeds that routes through
 * taskman's bounce copy (TM_REQ_MSG_XFER): MsgSend ships a 4-word
 * descriptor instead of the bytes; the server's MsgReceive PULLs the
 * payload from the client and MsgReply PUSHes the reply back.  The whole
 * buffer is copied verbatim (no word0/type split -- the inline path's
 * 32-bit-label type convention can't carry raw 64-bit data, and a copied
 * word0 is in fact more faithful than the truncating inline path). */
#define QSOE_MSG_INLINE_MAX   QSOE_MSG_MAX_BYTES   /* > this => bulk path   */
/* OR'd into the seL4 label to mark a bulk request.  App message types
 * occupy the low bits and must stay below this bit (they are small enums;
 * the inline path already truncates the label to 32 bits). */
#define QSOE_MSG_BULK_LABEL   0x40000000u
/* rcvid bit telling MsgReply the receive was bulk (so it PUSHes the reply
 * before answering).  Disjoint from QSOE_RCVID_SAVED (bit 31). */
#define QSOE_RCVID_BULK       0x40000000
#define QSOE_RCVID_PID_MASK   0x3fffffff   /* strip BULK + SAVED bits */

/* PULL: ask taskman to copy the blocked client's send buffer (client_src)
 * into our receive buffer (local_dst), and stash the client's reply
 * buffer for the matching PUSH.  Returns bytes copied or -1 (errno set). */
static long bulk_xfer_pull(pid_t client_pid, unsigned long client_src,
                           unsigned long local_dst, unsigned long len,
                           unsigned long client_rbuf, unsigned long client_rbytes)
{
    qsoe_ipcbuf->msg[4] = (seL4_Word)len;
    qsoe_ipcbuf->msg[5] = (seL4_Word)client_rbuf;
    qsoe_ipcbuf->msg[6] = (seL4_Word)client_rbytes;
    seL4_Word mr0 = TM_MSG_XFER_PULL, mr1 = (seL4_Word)client_pid;
    seL4_Word mr2 = (seL4_Word)client_src, mr3 = (seL4_Word)local_dst;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_MSG_XFER, 0, 0, 7);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }
    return (long)mr0;
}

/* PUSH: ask taskman to copy our reply buffer (local_src) into the client's
 * stashed reply buffer.  Returns bytes copied or -1 (errno set). */
static long bulk_xfer_push(pid_t client_pid, unsigned long local_src,
                           unsigned long len)
{
    seL4_Word mr0 = TM_MSG_XFER_PUSH, mr1 = (seL4_Word)client_pid;
    seL4_Word mr2 = (seL4_Word)local_src, mr3 = (seL4_Word)len;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_MSG_XFER, 0, 0, 4);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }
    return (long)mr0;
}

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

/* ---- The Msg* contract (kernel-agnostic, app-facing) ------------------
 *
 * Applications pass their OWN message structs; the IPC layer moves
 * opaque bytes and carries the type/status as call metadata, never as a
 * field the app pokes into a shared buffer:
 *
 *   - A REQUEST's first word is the message TYPE -- the application's
 *     own protocol field (QNX `msg.type`), NOT a transport "tag".  On
 *     LQ it rides in seL4's MessageInfo label internally; the seam puts
 *     it at word 0 of the receiver's buffer and lifts it from word 0 of
 *     the sender's, so neither side ever names the label.
 *   - A REPLY is pure payload.  Its status is an ARGUMENT to MsgReply
 *     and the RETURN VALUE of MsgSend -- it is never in the buffer.
 *
 * The legacy in-place mode (smsg/rmsg == NULL or == qsoe_ipcbuf) serves
 * only the OS-independent libc's own pre-pack callers (pathmgr client,
 * procmgr_detach); application code always supplies its own buffer and
 * never touches qsoe_ipcbuf. */

int MsgSend(int coid, const void *smsg, int sbytes,
            void *rmsg, int rbytes)
{
    qsoe_cancel_point();
    if (sbytes < 0 || rbytes < 0) { qsoe_errno = EINVAL; return -1; }
    seL4_CPtr send = qsoe_state_coid_to_slot(coid);
    if (!send) { qsoe_errno = EBADF; return -1; }

    /* Bulk path: either direction exceeds the message-register capacity.
     * Ship a descriptor {send buf, sbytes, reply buf, rbytes}; taskman
     * (driven by the server) copies the bytes.  Requires real caller
     * buffers (the legacy NULL/in-place mode is only ever small). */
    if ((sbytes > (int)QSOE_MSG_INLINE_MAX || rbytes > (int)QSOE_MSG_INLINE_MAX) &&
        smsg && smsg != (const void *)qsoe_ipcbuf &&
        rmsg && rmsg != (void *)qsoe_ipcbuf) {
        seL4_Word mr0 = (seL4_Word)(uintptr_t)smsg;
        seL4_Word mr1 = (seL4_Word)sbytes;
        seL4_Word mr2 = (seL4_Word)(uintptr_t)rmsg;
        seL4_Word mr3 = (seL4_Word)rbytes;
        seL4_MessageInfo_t tag =
            seL4_MessageInfo_new(QSOE_MSG_BULK_LABEL, 0, 0, 4);
        seL4_MessageInfo_t reply = qsoe_sys_call(send, tag,
                                                 &mr0, &mr1, &mr2, &mr3);
        /* The reply payload was PUSHed straight into rmsg by taskman
         * during the server's MsgReply; only the status rides the label. */
        return (int)seL4_MessageInfo_get_label(reply);
    }

    /* Request word 0 = type -> seL4 label; words 1+ = body -> MRs. */
    unsigned label;
    unsigned nwords;
    if (!smsg || smsg == (const void *)qsoe_ipcbuf) {
        label  = (unsigned)qsoe_ipcbuf->tag;          /* legacy pre-pack */
        nwords = pack_bytes(NULL, (unsigned)sbytes);
    } else {
        const unsigned long *req = smsg;
        label  = (unsigned)req[0];
        unsigned body = (sbytes >= 8) ? (unsigned)sbytes - 8 : 0;
        nwords = pack_bytes(&req[1], body);
    }
    seL4_Word mr0 = qsoe_ipcbuf->msg[0];
    seL4_Word mr1 = qsoe_ipcbuf->msg[1];
    seL4_Word mr2 = qsoe_ipcbuf->msg[2];
    seL4_Word mr3 = qsoe_ipcbuf->msg[3];

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(label, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(send, tag, &mr0, &mr1, &mr2, &mr3);

    /* Reply status = the seL4 label = MsgSend's return value (QNX
     * semantics); the reply body (MRs) is pure payload for the caller. */
    int status = (int)seL4_MessageInfo_get_label(reply);
    qsoe_ipcbuf->msg[0] = mr0;
    qsoe_ipcbuf->msg[1] = mr1;
    qsoe_ipcbuf->msg[2] = mr2;
    qsoe_ipcbuf->msg[3] = mr3;
    unsigned reply_bytes = (unsigned)seL4_MessageInfo_get_length(reply) * 8;
    if (rmsg && rmsg != (void *)qsoe_ipcbuf)
        unpack_bytes(rmsg, reply_bytes, (unsigned)rbytes);
    else
        qsoe_ipcbuf->tag = (unsigned long)status;     /* legacy readers */
    return status;
}

/* ---- MsgSendv / MsgSendvnc -- the vector (scatter/gather) forms ---------
 *
 * Skimmer (NQ) backs MsgSendv with an in-kernel sys_msg_sendv -- its
 * byte-copy IPC scatter-gathers IOVs in the kernel.  seL4 has no such
 * primitive (fixed message registers), but MsgSend already linearizes
 * every message through pack_bytes/unpack_bytes over the flat IPC buffer.
 * So a vector send is simply: GATHER the send IOVs into one contiguous
 * request image (word 0 = type -> seL4 label, the rest -> message regs),
 * one qsoe_sys_call, then SCATTER the pure-payload reply words back into
 * the recv IOVs.  This makes LQ's transport API a superset-match of NQ's,
 * which QNX userland needs (MsgSendv is a core QNX call).
 *
 * Inline frames only (gathered request / reply <= QSOE_MSG_MAX_BYTES);
 * a larger vector transfer returns -E2BIG (use MsgSend's single-buffer
 * bulk path for those).  The core returns the reply status (>=0) or a
 * negative errno -- the _r convention; the public wrappers map negatives
 * to errno + (-1), matching NQ. */
static long msg_sendv_core(int coid, const iov_t *siov, size_t sparts,
                           iov_t *riov, size_t rparts)
{
    seL4_CPtr send = qsoe_state_coid_to_slot(coid);
    if (!send) return -EBADF;

    /* Gather sends into one contiguous request image. */
    unsigned long sbuf[QSOE_MSG_MAX_BYTES / sizeof(unsigned long)];
    unsigned char *sb = (unsigned char *)sbuf;
    unsigned slen = 0;
    for (size_t i = 0; i < sparts; ++i) {
        const unsigned char *s = siov[i].iov_base;
        unsigned n = (unsigned)siov[i].iov_len;
        if (slen + n > sizeof sbuf) return -E2BIG;
        for (unsigned k = 0; k < n; ++k) sb[slen + k] = s[k];
        slen += n;
    }
    if (slen < sizeof(unsigned long)) return -EINVAL;   /* need the type word */

    /* Word 0 = type -> label; the remainder = body -> message registers. */
    unsigned label  = (unsigned)sbuf[0];
    unsigned nwords = pack_bytes(sb + sizeof(unsigned long),
                                 slen - (unsigned)sizeof(unsigned long));

    seL4_Word mr0 = qsoe_ipcbuf->msg[0];
    seL4_Word mr1 = qsoe_ipcbuf->msg[1];
    seL4_Word mr2 = qsoe_ipcbuf->msg[2];
    seL4_Word mr3 = qsoe_ipcbuf->msg[3];
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(label, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(send, tag, &mr0, &mr1, &mr2, &mr3);

    int status = (int)seL4_MessageInfo_get_label(reply);
    qsoe_ipcbuf->msg[0] = mr0;
    qsoe_ipcbuf->msg[1] = mr1;
    qsoe_ipcbuf->msg[2] = mr2;
    qsoe_ipcbuf->msg[3] = mr3;

    /* Scatter the reply (pure payload, contiguous from word 0) into riov. */
    unsigned avail = (unsigned)seL4_MessageInfo_get_length(reply)
                     * (unsigned)sizeof(unsigned long);
    if (avail > QSOE_MSG_MAX_BYTES) avail = QSOE_MSG_MAX_BYTES;
    const unsigned char *r = (const unsigned char *)qsoe_ipcbuf->msg;
    unsigned off = 0;
    for (size_t i = 0; i < rparts; ++i) {
        unsigned char *d = riov[i].iov_base;
        unsigned n = (unsigned)riov[i].iov_len;
        for (unsigned k = 0; k < n && off < avail; ++k) d[k] = r[off++];
    }
    return status;
}

long MsgSendv_r(int coid, const iov_t *siov, size_t sparts,
                iov_t *riov, size_t rparts)
{
    return msg_sendv_core(coid, siov, sparts, riov, rparts);
}

long MsgSendv(int coid, const iov_t *siov, size_t sparts,
              iov_t *riov, size_t rparts)
{
    qsoe_cancel_point();
    long r = msg_sendv_core(coid, siov, sparts, riov, rparts);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return r;
}

long MsgSendvnc_r(int coid, const iov_t *siov, size_t sparts,
                  iov_t *riov, size_t rparts)
{
    return msg_sendv_core(coid, siov, sparts, riov, rparts);
}

long MsgSendvnc(int coid, const iov_t *siov, size_t sparts,
                iov_t *riov, size_t rparts)
{
    long r = msg_sendv_core(coid, siov, sparts, riov, rparts);
    if (r < 0) { qsoe_errno = (int)-r; return -1; }
    return r;
}

int MsgReceive(int chid, void *msg, int bytes, struct _msg_info *info)
{
    qsoe_cancel_point();
    if (bytes < 0) { qsoe_errno = EINVAL; return -1; }
    seL4_CPtr recv = qsoe_state_chid_to_slot(chid);
    if (!recv) { qsoe_errno = EBADF; return -1; }

    seL4_Word badge;
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    /* MCS: the receive carries THIS thread's own reply object (register
     * a6), which the kernel binds to the incoming Call so MsgReply can
     * answer it.  Reply objects cannot be shared between threads -- the
     * main thread uses QSOE_CAP_REPLY (taskman-provisioned at spawn),
     * every worker/system thread the one ThreadCreate gave it.  Sharing
     * one across two receivers trips seL4's "Reply object already has
     * unexecuted reply!" and the second receive never completes. */
    seL4_CPtr reply_cap = (seL4_CPtr)qsoe_curthr()->reply_cap;
    if (!reply_cap) reply_cap = QSOE_CAP_REPLY;   /* defensive: pre-init */
    seL4_MessageInfo_t tag = qsoe_sys_recv(recv, &badge, reply_cap,
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

    /* The request TYPE travels in seL4's MessageInfo label; the body is
     * in the MRs (MRs 4+ already in qsoe_ipcbuf == the seL4 IPC buffer).
     * Hand the application its message: word 0 = type, words 1+ = body.
     * The app reads its own struct's `type` field -- it never sees the
     * label or qsoe_ipcbuf. */
    unsigned label = (unsigned)seL4_MessageInfo_get_label(tag);

    /* Bulk receive: the message is a descriptor, not the payload.  PULL
     * the client's send buffer into the caller's `msg` buffer via taskman
     * and stash the client's reply buffer for the matching MsgReply.  The
     * descriptor MRs are {client send buf, sbytes, client reply buf,
     * rbytes}; the badge is the client pid. */
    if (label & QSOE_MSG_BULK_LABEL) {
        unsigned long c_sbuf   = (unsigned long)mr0;
        unsigned long c_sbytes = (unsigned long)mr1;
        unsigned long c_rbuf   = (unsigned long)mr2;
        unsigned long c_rbytes = (unsigned long)mr3;
        unsigned long want = ((unsigned long)bytes < c_sbytes)
                             ? (unsigned long)bytes : c_sbytes;
        if (msg && msg != (void *)qsoe_ipcbuf && want > 0) {
            long copied = bulk_xfer_pull((pid_t)badge, c_sbuf,
                                         (unsigned long)(uintptr_t)msg, want,
                                         c_rbuf, c_rbytes);
            if (copied < 0) return -1;   /* errno set by bulk_xfer_pull */
        } else {
            /* No buffer to fill, but still record the reply target so a
             * (small or empty) MsgReply can complete the client. */
            (void)bulk_xfer_pull((pid_t)badge, c_sbuf,
                                 (unsigned long)(uintptr_t)msg, 0,
                                 c_rbuf, c_rbytes);
        }
        if (info) {
            info->nd        = ND_LOCAL_NODE;
            info->pid       = (pid_t)badge;
            info->chid      = chid;
            info->scoid     = (int)badge;
            info->coid      = 0;
            info->msglen    = (int)c_sbytes;
            info->srcmsglen = (int)c_sbytes;
            info->dstmsglen = bytes;
            info->priority  = 0;
            info->flags     = 0;
            info->label     = 0;
        }
        return (int)(QSOE_RCVID_BULK | (unsigned)badge);
    }

    qsoe_ipcbuf->msg[0] = mr0;
    qsoe_ipcbuf->msg[1] = mr1;
    qsoe_ipcbuf->msg[2] = mr2;
    qsoe_ipcbuf->msg[3] = mr3;

    unsigned in_bytes = (unsigned)seL4_MessageInfo_get_length(tag) * 8;
    if (msg && msg != (void *)qsoe_ipcbuf) {
        unsigned long *m = msg;
        m[0] = label;                            /* word 0 = type */
        unsigned cap = (bytes >= 8) ? (unsigned)bytes - 8 : 0;
        /* body words 1+ come from the MRs (now in qsoe_ipcbuf->msg) */
        unsigned n = in_bytes < cap ? in_bytes : cap;
        if (n > QSOE_MSG_MAX_BYTES) n = QSOE_MSG_MAX_BYTES;
        const unsigned char *s = (const unsigned char *)qsoe_ipcbuf->msg;
        unsigned char *d = (unsigned char *)&m[1];
        for (unsigned i = 0; i < n; ++i) d[i] = s[i];
    } else {
        qsoe_ipcbuf->tag = label;                /* legacy in-place */
    }

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
        info->label     = label;
    }

    /* rcvid: in QNX it's a token identifying this specific receive.
     * Under MCS the reply travels via this thread's reply object, so the
     * rcvid is for the client→server protocol's benefit. We return the
     * badge: it identifies the sender, and the MsgReply that consumes the
     * rcvid replies on the active reply object. */
    return (int)badge;
}

int MsgSavereply(int rcvid)
{
    /* Already saved — caller is double-saving; just hand the same
     * stable rcvid back. */
    if ((unsigned)rcvid & QSOE_RCVID_SAVED) return rcvid;

    unsigned long slot = qsoe_state_alloc_empty_slot();
    if (!slot) { qsoe_errno = EAGAIN; return -1; }
    /* MCS: stash this thread's reply object (bound to the client by the
     * preceding MsgReceive) into `slot`, then replenish QSOE_CAP_REPLY
     * with a fresh reply object retyped from our own untyped budget so
     * the next MsgReceive has one.  Replaces the non-MCS SaveCaller. */
    if (qsoe_cnode_move(QSOE_CAP_CNODE_SELF, slot, QSOE_CAP_CNODE_DEPTH,
                        QSOE_CAP_CNODE_SELF, QSOE_CAP_REPLY,
                        QSOE_CAP_CNODE_DEPTH) != 0) {
        qsoe_state_free_empty_slot(slot);
        qsoe_errno = ENOMEM;
        return -1;
    }
    if (qsoe_untyped_retype(QSOE_CAP_OWN_UNTYPED, seL4_ReplyObject, 0,
                            QSOE_CAP_CNODE_SELF, 0, 0,
                            QSOE_CAP_REPLY, 1) != 0) {
        /* Replenish failed — move the client's reply back so it isn't
         * stranded, and fail the save. */
        qsoe_cnode_move(QSOE_CAP_CNODE_SELF, QSOE_CAP_REPLY,
                        QSOE_CAP_CNODE_DEPTH,
                        QSOE_CAP_CNODE_SELF, slot, QSOE_CAP_CNODE_DEPTH);
        qsoe_state_free_empty_slot(slot);
        qsoe_errno = ENOMEM;
        return -1;
    }
    return (int)(QSOE_RCVID_SAVED | (unsigned)slot);
}

int MsgReply(int rcvid, int status, const void *msg, int bytes)
{
    qsoe_cancel_point();
    if (bytes < 0) { qsoe_errno = EINVAL; return -1; }

    /* Bulk reply: the matching MsgReceive was bulk, so PUSH the reply
     * payload into the client's reply buffer (taskman holds its address)
     * before answering.  Then the seL4 reply carries only the status --
     * the client's MsgSend already has its data once it unblocks. */
    if ((unsigned)rcvid & QSOE_RCVID_BULK) {
        pid_t client_pid = (pid_t)((unsigned)rcvid & QSOE_RCVID_PID_MASK);
        if (msg && msg != (const void *)qsoe_ipcbuf && bytes > 0) {
            if (bulk_xfer_push(client_pid, (unsigned long)(uintptr_t)msg,
                               (unsigned long)bytes) < 0)
                return -1;   /* errno set by bulk_xfer_push */
        } else {
            /* Empty/no-payload reply: clear the stash so taskman doesn't
             * leak the pending entry. */
            (void)bulk_xfer_push(client_pid, 0, 0);
        }
        seL4_MessageInfo_t btag =
            seL4_MessageInfo_new((unsigned)status, 0, 0, 0);
        qsoe_sys_send(QSOE_CAP_REPLY, btag, 0, 0, 0, 0);
        return 0;
    }

    /* The reply is PURE PAYLOAD: the whole caller buffer (from word 0)
     * becomes the reply body MRs.  `status` is the reply metadata -> the
     * seL4 label -> the client's MsgSend return value.  No tag slot. */
    unsigned nwords = (msg && msg != (const void *)qsoe_ipcbuf)
                      ? pack_bytes(msg, (unsigned)bytes)
                      : pack_bytes(NULL, (unsigned)bytes);
    seL4_Word mr0 = qsoe_ipcbuf->msg[0];
    seL4_Word mr1 = qsoe_ipcbuf->msg[1];
    seL4_Word mr2 = qsoe_ipcbuf->msg[2];
    seL4_Word mr3 = qsoe_ipcbuf->msg[3];

    seL4_MessageInfo_t tag = seL4_MessageInfo_new((unsigned)status,
                                                   0, 0, nwords);
    if ((unsigned)rcvid & QSOE_RCVID_SAVED) {
        /* Deferred reply: Send to the stashed reply object (unblocks the
         * client), then delete it and recycle the slot.  An MCS reply
         * object is not self-cleared by the Send, so delete it. */
        unsigned long slot = (unsigned long)((unsigned)rcvid & ~QSOE_RCVID_SAVED);
        qsoe_sys_send((seL4_CPtr)slot, tag, mr0, mr1, mr2, mr3);
        qsoe_cnode_delete(QSOE_CAP_CNODE_SELF, slot, QSOE_CAP_CNODE_DEPTH);
        qsoe_state_free_empty_slot(slot);
    } else {
        /* Normal reply: Send to this thread's reply object, which the
         * matching MsgReceive bound to the client.  The object is reused
         * (re-armed) by the next MsgReceive. */
        qsoe_sys_send(QSOE_CAP_REPLY, tag, mr0, mr1, mr2, mr3);
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
