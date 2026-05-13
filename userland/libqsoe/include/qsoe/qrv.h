/*
 * <qsoe/qrv.h> — QNX-compatible IPC API (libqsoe public surface).
 *
 * v0.2: only the four channel/connection lifecycle calls. The MsgSend /
 * MsgReceive / MsgReply family lands in v0.3+.
 *
 * All four functions return -1 on failure and set errno (not yet
 * implemented as a real TLS errno; for now a plain global). The error
 * codes are the QNX-compatible subset listed below.
 */
#ifndef QSOE_QRV_H
#define QSOE_QRV_H

typedef int           pid_t;
typedef unsigned int  uint32_t;

#define ND_LOCAL_NODE 0

/* Error codes (QNX-compatible subset for v0.x). */
#define EOK            0
#define ESRCH          3
#define EBADF          9
#define ENOMEM        12
#define EINVAL        22
#define EHOSTUNREACH 113

extern int qsoe_errno;

int ChannelCreate(unsigned flags);
int ChannelDestroy(int chid);
int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags);
int ConnectDetach(int coid);

/*
 * Message-passing primitives. QNX terminology + semantics:
 *   MsgSend     synchronous send-and-receive; blocks until reply
 *   MsgReceive  block on a channel until a sender shows up
 *   MsgReply    reply to a previously-received message
 *
 * Byte payloads up to 960 bytes (seL4's IPC buffer capacity) fit
 * directly in the IPC buffer. v0.4+ adds shared-frame channels for
 * larger transfers.
 *
 * Non-MCS reply caveat: on seL4 non-MCS the reply capability is a
 * single per-thread slot (tcbCaller). A server MUST reply to each
 * received message before receiving the next, or the older reply cap
 * is dropped. The taskman dispatch loop uses MsgReply followed by
 * MsgReceive (or the merged ReplyRecv path) to honour this. v0.4
 * adds seL4_CNode_SaveCaller for queued/delayed replies.
 */

/* Minimal QNX-compatible _msg_info subset. v0.4 grows the rest. */
struct _msg_info {
    uint32_t nd;        /* sending node (always ND_LOCAL_NODE in v0.x) */
    pid_t    pid;       /* sender's pid (badge, in our mapping) */
    int      chid;      /* channel the message arrived on */
    int      scoid;     /* server-side connection id (== badge for v0.x) */
    int      coid;      /* sender's connection id (unknown server-side) */
    int      msglen;    /* total bytes the sender sent (clamped to buf) */
    int      srcmsglen; /* same as msglen until v0.4 */
    int      dstmsglen; /* requested receive size */
    int      priority;  /* (unused; v0.4) */
    int      flags;     /* (unused; v0.4) */
};

int MsgSend(int coid, const void *smsg, int sbytes,
            void *rmsg, int rbytes);
int MsgReceive(int chid, void *msg, int bytes, struct _msg_info *info);
int MsgReply(int rcvid, int status, const void *msg, int bytes);

#endif /* QSOE_QRV_H */
