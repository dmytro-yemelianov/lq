/*
 * server.h — taskman's internal handler API for the QNX-style IPC
 * lifecycle calls + connection introspection, plus the data model for
 * processes, channels, and connections.
 *
 * libqsoe sees this header only when compiled with
 * -DQSOE_LIBQSOE_IN_TASKMAN (the build flag that turns the libqsoe
 * entrypoints into direct calls into these handlers).
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
    seL4_CPtr tcb;          /* (kept for v0.4 cleanup; unused in v0.3.x) */
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
    unsigned  flags;         /* v0.3.3: ConnectFlags state (COF_*) */
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

/* Lookup helper used by spawn.c — returns the channel-table index for
 * (pid, chid), or -1 if not found. Index, not pointer, because
 * tm_connection_register_existing stores the index. */
int tm_channel_index(pid_t pid, int chid);

/* Register a connection that was minted outside ConnectAttach (e.g.
 * spawn.c minting SYSMGR_COID into a child's slot 1). Returns 0 on
 * success. */
int tm_connection_register_existing(pid_t client_pid, seL4_CPtr client_slot,
                                    int channel_idx, seL4_Word badge,
                                    unsigned flags);

/* ----------- lifecycle handlers (v0.2) ----------- */

int tm_channel_create(pid_t owner_pid, int chid, unsigned flags,
                      seL4_CPtr *out_recv_slot);
int tm_channel_destroy(pid_t owner_pid, seL4_CPtr recv_slot);
int tm_connect_attach(pid_t client_pid, pid_t target_pid, int target_chid,
                      unsigned flags, seL4_CPtr *out_send_slot);
int tm_connect_detach(pid_t client_pid, seL4_CPtr send_slot);

/* ----------- introspection handlers (v0.3.3) ----------- */

/* ConnectServerInfo backend. caller_pid + client_slot identifies the
 * connection in taskman's registry; out_* receive the server side's
 * identity. Returns 0 / -errno. */
int tm_connect_server_info(pid_t caller_pid, seL4_CPtr client_slot,
                           pid_t *out_server_pid, int *out_server_chid,
                           seL4_Word *out_scoid);

/* ConnectClientInfo backend. scoid (== badge in v0.3.3) identifies the
 * connection; out_* describe the client. */
int tm_connect_client_info(seL4_Word scoid,
                           pid_t *out_client_pid, pid_t *out_sid,
                           unsigned *out_flags);

/* ConnectFlags backend. Atomic flag mutation; returns the *previous*
 * flag value in *out_old. mask==0 → pure query. */
int tm_connect_flags(pid_t caller_pid, seL4_CPtr client_slot,
                     unsigned mask, unsigned bits,
                     unsigned *out_old);

#endif /* QSOE_TASKMAN_SERVER_H */
