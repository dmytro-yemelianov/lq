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
 * compatibility — internally it's the implicit reply cap. See qrv.h
 * for the caveat.
 *
 * Byte ↔ word marshalling: we treat the IPC buffer's msg[] array as
 * a flat byte buffer. msg[0..3] is reserved for the kernel's
 * "fast-path" register slots (a2-a5) — on send we read from msg[0..3]
 * after packing, on receive we copy the in-register reply MRs back
 * into msg[0..3] so unpack_bytes can find them.
 */

#include "../include/qsoe/qrv.h"
#include "../include/qsoe/slots.h"
#include "state.h"

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
 * For non-taskman processes this also pre-binds SYSMGR_COID to
 * QSOE_CAP_TASKMAN_EP. spawn.c minted the cap into that CSpace slot at
 * spawn time, so the connection is already live — we just teach
 * libqsoe's coid table about it. */
void qsoe_libqsoe_init(void *ipcbuf, pid_t self_pid)
{
    qsoe_curthr()->ipcbuf   = ipcbuf;
    qsoe_curthr()->self_pid = self_pid;
    if (self_pid != QSOE_PID_TASKMAN) {
        qsoe_state_bind_coid(SYSMGR_COID, QSOE_CAP_TASKMAN_EP);
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
 * the number of seL4 words occupied (= ceil(nbytes/8)). */
static unsigned pack_bytes(const void *src, unsigned nbytes)
{
    if (nbytes > QSOE_MSG_MAX_BYTES) nbytes = QSOE_MSG_MAX_BYTES;
    unsigned nwords = (nbytes + 7) / 8;
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

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(0, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(send, tag, &mr0, &mr1, &mr2, &mr3);

    /* Stage reply MRs back into msg[0..3] for unpack_bytes. */
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
    }

    /* rcvid: in QNX it's a token identifying this specific receive.
     * On non-MCS seL4 the reply cap is implicit (one per server
     * thread), so the rcvid is purely for the client→server protocol's
     * benefit. We return the badge: it identifies the sender, and the
     * MsgReply that consumes the rcvid uses the implicit reply cap. */
    return (int)badge;
}

int MsgReply(int rcvid, int status, const void *msg, int bytes)
{
    (void)rcvid;  /* non-MCS: implicit reply cap, not addressed by rcvid */
    qsoe_cancel_point();
    if (bytes < 0) { qsoe_errno = EINVAL; return -1; }

    unsigned nwords = pack_bytes(msg, (unsigned)bytes);
    seL4_Word mr0 = qsoe_ipcbuf->msg[0];
    seL4_Word mr1 = qsoe_ipcbuf->msg[1];
    seL4_Word mr2 = qsoe_ipcbuf->msg[2];
    seL4_Word mr3 = qsoe_ipcbuf->msg[3];

    seL4_MessageInfo_t tag = seL4_MessageInfo_new((unsigned)status,
                                                   0, 0, nwords);
    qsoe_sys_reply(tag, mr0, mr1, mr2, mr3);
    return 0;
}
