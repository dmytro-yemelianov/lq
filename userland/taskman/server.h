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
#define TM_MAX_PROCESSES    8

typedef struct {
    int       in_use;
    pid_t     pid;
    seL4_CPtr cnode;        /* caller's root CNode cap in taskman's CSpace */
    seL4_CPtr next_slot;    /* next free slot in caller's CSpace */
    seL4_CPtr tcb;          /* (kept for v0.4 cleanup; unused in v0.3.2) */
    seL4_CPtr vspace;
} tm_process_t;

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

/* Process table accessors. tm_init registers taskman itself as pid 1
 * with cnode = seL4_CapInitThreadCNode. tm_spawn registers each child
 * after a successful spawn. */
int            tm_process_register(pid_t pid, seL4_CPtr cnode,
                                   seL4_CPtr tcb, seL4_CPtr vspace,
                                   seL4_CPtr first_free_slot);
tm_process_t  *tm_process_lookup(pid_t pid);
seL4_CPtr      tm_process_alloc_slot(pid_t pid);

/* Register an externally-allocated endpoint as channel (pid, chid).
 * Used for taskman's primary endpoint, which is retyped at boot before
 * tm_init's registry is ready to retype anything itself. master_slot
 * and recv_slot may be the same (taskman uses one cap for both
 * directions). Returns 0 on success, negative on failure. */
int tm_channel_register_existing(pid_t pid, int chid,
                                 seL4_CPtr master_slot,
                                 seL4_CPtr recv_slot);

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
