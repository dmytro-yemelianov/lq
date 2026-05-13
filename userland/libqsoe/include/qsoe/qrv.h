/*
 * <qsoe/qrv.h> — QNX-compatible IPC API (libqsoe public surface).
 *
 * v0.3.3: + side-channel coid/chid namespace + SYSMGR_*
 *         + ConnectServerInfo / ConnectClientInfo / ConnectFlags.
 *
 * All entrypoints return -1 on failure and set qsoe_errno to a QNX-
 * compatible value.
 */
#ifndef QSOE_QRV_H
#define QSOE_QRV_H

#include <qsoe/tls.h>   /* qsoe_errno, qsoe_self_pid, qsoe_ipcbuf, pid_t */

typedef unsigned int  uint32_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;

#define ND_LOCAL_NODE 0

/*
 * Side-channel namespace marker. Set on a coid (or chid) returned by
 * ChannelCreate / ConnectAttach when the QSOE_SIDE_CHANNEL flag is
 * passed. The marker lives in bit 30 so the value stays a positive
 * signed int while remaining unambiguously disjoint from the FD coid
 * range (0..N).
 *
 * Rationale (matches QNX _NTO_SIDE_CHANNEL): in QNX-like systems an
 * `int fd` returned by open() is the same integer as the coid the
 * filesystem-or-resmgr connection lives at. fds 0/1/2 are coids
 * 0/1/2. If the system-process connection were a low-numbered coid it
 * would alias stdin/stdout/stderr. Putting it in side-channel space
 * keeps it out of FD allocation.
 */
#define QSOE_SIDE_CHANNEL   0x40000000u

/* The system process (taskman) sits at a fixed pid/chid and is reached
 * by every process via a pre-bound side-channel coid. spawn.c mints
 * the cap at CSpace slot QSOE_CAP_TASKMAN_EP and the libqsoe init hook
 * binds it to SYSMGR_COID. */
#define SYSMGR_PID          1
#define SYSMGR_CHID         1
#define SYSMGR_COID         ((int)QSOE_SIDE_CHANNEL)
#define SYSMGR_HANDLE       0

/* ConnectAttach / ConnectFlags flag bits (mirrors QRV_FLG_COF_*). */
#define QSOE_COF_CLOEXEC    0x0001u
#define QSOE_COF_DEAD       0x0002u
#define QSOE_COF_NOSHARE    0x0040u
#define QSOE_COF_NONBLOCK   0x0100u

/* Error codes (QNX-compatible subset for v0.x). */
#define EOK            0
#define ESRCH          3
#define EBADF          9
#define ENOMEM        12
#define EINVAL        22
#define ENOSYS        89
#define EHOSTUNREACH 113

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

/* QNX uses `_server_info` and `_msg_info` interchangeably; the former
 * is the field-for-field subset filled by ConnectServerInfo while the
 * latter is what MsgReceive fills. Same shape, single alias. */
#define _server_info _msg_info

struct _cred_info {
    uid_t  ruid, euid, suid;
    gid_t  rgid, egid, sgid;
    uint32_t ngroups;
    /* In QNX a flexible array `gid_t grouplist[ngroups]` follows here;
     * QSOE has no users/groups yet so ngroups is always 0 in v0.3.3. */
};

struct _client_info {
    uint32_t nd;
    pid_t    pid;
    pid_t    sid;
    uint32_t flags;
    struct _cred_info cred;
};

int MsgSend(int coid, const void *smsg, int sbytes,
            void *rmsg, int rbytes);
int MsgReceive(int chid, void *msg, int bytes, struct _msg_info *info);
int MsgReply(int rcvid, int status, const void *msg, int bytes);

/*
 * Connection introspection / control.
 *
 *   ConnectServerInfo  — client side: who is on the other end of `coid`?
 *                        Fills server's nd/pid/chid/scoid; coid echoes
 *                        the argument. v0.3.3 requires pid==0
 *                        (caller's own process); ENOSYS otherwise.
 *
 *   ConnectClientInfo  — server side: who is the client behind `scoid`
 *                        (the value MsgReceive returned in
 *                        info->scoid)? Fills client's nd/pid/sid/flags;
 *                        cred is zeroed in v0.3.3 (no users yet).
 *                        `ngroups` is the caller's grouplist[] capacity;
 *                        ignored until v0.4+ adds groups.
 *
 *   ConnectFlags       — atomic flag get/set on a connection.
 *                        mask==0 → pure query (no mutation). Returns
 *                        the *previous* flag value, or -1 on error.
 */
int ConnectServerInfo(pid_t pid, int coid, struct _server_info *info);
int ConnectClientInfo(int scoid, struct _client_info *info, int ngroups);
int ConnectFlags(pid_t pid, int coid, unsigned mask, unsigned bits);

/*
 * Threading (v0.4). QNX/QRV-compatible surface.
 *
 *   ThreadCreate(pid, func, arg, attr) — spawn a new thread in the
 *     given process. pid==0 means "this process"; cross-process
 *     ThreadCreate returns ENOSYS in v0.4. Returns the new tid (>= 2)
 *     or -1.
 *
 *   ThreadDestroy(tid, prio, status) — terminate a thread, set its
 *     exit_status (read by ThreadJoin), revoke its TCB. Calling with
 *     tid==0 means "current thread"; the trampoline does this on
 *     return from the user func.
 *
 *   ThreadDetach(tid) — set the detached flag; once set, the thread's
 *     resources are reclaimed when it exits and ThreadJoin fails.
 *
 *   ThreadJoin(tid, *status) — block until the thread exits; receive
 *     its exit_status.
 *
 *   ThreadCancel(tid, canstub) — request termination. v0.4 supports
 *     only deferred cancellation: the target observes cancel_pending
 *     at the next libqsoe entry point (MsgSend / MsgReceive / etc).
 *     `canstub` is stored but unused in v0.4.
 *
 *   ThreadCtl(cmd, data) — control op. v0.4 supports QSOE_TCTL_NAME
 *     and QSOE_TCTL_RUNMASK; others return ENOSYS.
 */
typedef unsigned long size_t;

#define QSOE_PTHREAD_CREATE_JOINABLE  0
#define QSOE_PTHREAD_CREATE_DETACHED  1

struct _thread_attr {
    int        flags;       /* QSOE_PTHREAD_CREATE_DETACHED bit */
    size_t     stacksize;   /* if 0 → libqsoe default (56 KiB) */
    void      *stackaddr;   /* if NULL → libqsoe allocates */
    void     (*exitfunc)(void *status);   /* stored, not yet wired in v0.4 */
    int        policy;
    int        prio;        /* 1..255; 0 → default 254 */
    unsigned   runmask;     /* SMP affinity bitmask; 0 → CPU 0 */
    unsigned   guardsize;
    unsigned   prealloc;
};

#define QSOE_TCTL_NAME     11
#define QSOE_TCTL_RUNMASK   4

int ThreadCreate(pid_t pid, void *(*func)(void *), void *arg,
                 const struct _thread_attr *attr);
int ThreadDestroy(int tid, int priority, void *status);
int ThreadDetach(int tid);
int ThreadJoin(int tid, void **status);
int ThreadCancel(int tid, void (*canstub)(void));
int ThreadCtl(int cmd, void *data);

/*
 * libqsoe init hook. Each spawned process calls this exactly once at
 * startup with its IPC-buffer pointer and pid:
 *   - records the pointer so msg.c can pack into the buffer;
 *   - records self_pid for ConnectServerInfo's pid==0 shortcut;
 *   - for non-taskman processes, pre-binds SYSMGR_COID → CSpace slot
 *     QSOE_CAP_TASKMAN_EP (which spawn.c minted at child startup).
 */
void qsoe_libqsoe_init(void *ipcbuf, pid_t self_pid);

#endif /* QSOE_QRV_H */
