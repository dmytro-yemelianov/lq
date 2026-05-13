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

#endif /* QSOE_QRV_H */
