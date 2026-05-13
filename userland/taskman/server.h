/*
 * server.h — taskman's internal handler API for the four core IPC
 * lifecycle calls, plus the data model for channels and connections.
 *
 * libqsoe sees this header only when compiled with
 * -DQSOE_LIBQSOE_IN_TASKMAN (the build flag that turns the four
 * libqsoe entrypoints into direct calls into these handlers).
 *
 * See Design doc §3.4 "Channel and connection registries" and §4.3
 * "State split: libqsoe vs taskman".
 */
#ifndef QSOE_TASKMAN_SERVER_H
#define QSOE_TASKMAN_SERVER_H

#include "sel4_types.h"
#include "../libqsoe/include/qsoe/qrv.h"

#define TM_MAX_CHANNELS    64
#define TM_MAX_CONNECTIONS 256

typedef struct {
    int       in_use;
    seL4_CPtr master;        /* taskman-side master cap slot */
    seL4_CPtr owner_recv;    /* owner's recv-cap slot (libqsoe's handle) */
    pid_t     owner_pid;
    int       owner_chid;
    unsigned  flags;
} tm_channel_t;

typedef struct {
    int       in_use;
    int       channel_idx;   /* into the channel table */
    seL4_Word badge;
    pid_t     client_pid;
    seL4_CPtr client_slot;   /* the slot the client received */
} tm_connection_t;

/* Initialise taskman's allocators and registries.
 *   ut         — a non-device untyped to retype objects out of
 *   cnode_root — the caller's (taskman's) root CNode cap
 *   first_free — first free slot in that CNode (bump allocator origin)
 */
void tm_init(seL4_CPtr ut, seL4_CPtr cnode_root, seL4_CPtr first_free);

/* Handler: create a channel. Owner_pid is taskman's own pid for v0.2;
 * future versions take it from the IPC sender's badge. Returns 0 on
 * success and writes the slot of the recv-side copy into *out_recv_slot;
 * negative QNX errno on failure. */
int tm_channel_create(pid_t owner_pid, int chid, unsigned flags,
                      seL4_CPtr *out_recv_slot);

/* Handler: destroy a channel. recv_slot identifies the channel via the
 * caller's recv-cap slot (the value returned by tm_channel_create). */
int tm_channel_destroy(pid_t owner_pid, seL4_CPtr recv_slot);

/* Handler: attach a connection. Mints a badged Send cap into a fresh
 * slot in the caller's CSpace; *out_send_slot returns the slot. */
int tm_connect_attach(pid_t client_pid, pid_t target_pid, int target_chid,
                      unsigned flags, seL4_CPtr *out_send_slot);

/* Handler: detach a connection identified by the caller's send-cap slot. */
int tm_connect_detach(pid_t client_pid, seL4_CPtr send_slot);

#endif /* QSOE_TASKMAN_SERVER_H */
