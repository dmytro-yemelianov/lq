/*
 * server.c — taskman's process / channel / connection registries and the
 * four lifecycle handler functions invoked by libqsoe (locally via the
 * IN_TASKMAN shortcut, or remotely via the dispatch loop in main.c).
 *
 * v0.x simplifications:
 *   - Flat arrays with linear scan for all registries.
 *   - Bump slot allocator over the caller's CNode (no recycling).
 *   - Single backing untyped (the largest non-device one).
 *   - Single-threaded; no locking.
 *
 * See Design doc §3.4 and §4.3.
 */

#include "server.h"
#include "qsoe_invoke.h"
#include "../libqsoe/include/qsoe/slots.h"

static tm_channel_t    g_channels[TM_MAX_CHANNELS];
static tm_connection_t g_connections[TM_MAX_CONNECTIONS];
static tm_process_t    g_processes[TM_MAX_PROCESSES];

/* Shared with spawn.c — kept non-static for v0.3.0 simplicity; v0.4
 * wraps these in a proper accessor API. */
seL4_CPtr s_untyped;
seL4_CPtr s_cnode_root;
seL4_CPtr s_next_slot;
static seL4_Word s_next_badge = 1; /* 0 reserved as "no badge" */

/* CNode lookup depth for each process. taskman's own initThreadCNode
 * comes with a built-in guard such that the effective depth is 64.
 * Spawned children's CNodes are freshly retyped — no guard, so we use
 * the radix (12). The guard the children themselves see at runtime is
 * set by TCB_Configure's cnode_data field; that's independent. */
#define TM_DEPTH_TASKMAN 64
#define TM_DEPTH_CHILD   12

void tm_init(seL4_CPtr ut, seL4_CPtr cnode_root, seL4_CPtr first_free)
{
    s_untyped    = ut;
    s_cnode_root = cnode_root;
    s_next_slot  = first_free;

    /* Register taskman itself as pid 1. */
    g_processes[0].in_use    = 1;
    g_processes[0].pid       = QSOE_PID_TASKMAN;
    g_processes[0].cnode     = cnode_root;
    g_processes[0].next_slot = first_free;  /* shares s_next_slot's view */
    g_processes[0].tcb       = seL4_CapInitThreadTCB;
    g_processes[0].vspace    = seL4_CapInitThreadVSpace;
}

int tm_process_register(pid_t pid, seL4_CPtr cnode,
                        seL4_CPtr tcb, seL4_CPtr vspace,
                        seL4_CPtr first_free_slot)
{
    if (tm_process_lookup(pid)) return -EINVAL;
    for (int i = 0; i < TM_MAX_PROCESSES; ++i) {
        if (g_processes[i].in_use) continue;
        g_processes[i].in_use    = 1;
        g_processes[i].pid       = pid;
        g_processes[i].cnode     = cnode;
        g_processes[i].next_slot = first_free_slot;
        g_processes[i].tcb       = tcb;
        g_processes[i].vspace    = vspace;
        return 0;
    }
    return -ENOMEM;
}

tm_process_t *tm_process_lookup(pid_t pid)
{
    for (int i = 0; i < TM_MAX_PROCESSES; ++i) {
        if (g_processes[i].in_use && g_processes[i].pid == pid) {
            return &g_processes[i];
        }
    }
    return 0;
}

seL4_CPtr tm_process_alloc_slot(pid_t pid)
{
    tm_process_t *p = tm_process_lookup(pid);
    if (!p) return 0;
    /* For taskman, the global s_next_slot is the source of truth
     * (spawn.c bumps it directly). Keep p->next_slot in sync. */
    if (p->pid == QSOE_PID_TASKMAN) {
        return s_next_slot++;
    }
    return p->next_slot++;
}

static seL4_Uint8 cnode_depth_for(pid_t pid)
{
    return (pid == QSOE_PID_TASKMAN) ? TM_DEPTH_TASKMAN : TM_DEPTH_CHILD;
}

/* Allocate one untyped retype into the next free slot of taskman's
 * CSpace. Master caps always live in taskman's CSpace regardless of
 * who created the channel. */
static seL4_CPtr taskman_alloc_and_retype(seL4_Word type, seL4_Word size_bits)
{
    seL4_CPtr slot = s_next_slot++;
    seL4_Word err = qsoe_untyped_retype(s_untyped, type, size_bits,
                                         s_cnode_root, 0, 0, slot, 1);
    return (err == 0) ? slot : 0;
}

static tm_channel_t *channel_find(pid_t pid, int chid)
{
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        tm_channel_t *c = &g_channels[i];
        if (c->in_use && c->owner_pid == pid && c->owner_chid == chid) {
            return c;
        }
    }
    return 0;
}

int tm_channel_register_existing(pid_t pid, int chid,
                                 seL4_CPtr master_slot,
                                 seL4_CPtr recv_slot)
{
    if (!tm_process_lookup(pid)) return -ESRCH;
    if (channel_find(pid, chid)) return -EINVAL;
    int idx = -1;
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (!g_channels[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return -ENOMEM;
    g_channels[idx].in_use     = 1;
    g_channels[idx].master     = master_slot;
    g_channels[idx].owner_recv = recv_slot;
    g_channels[idx].owner_pid  = pid;
    g_channels[idx].owner_chid = chid;
    g_channels[idx].flags      = 0;
    return 0;
}

int tm_channel_index(pid_t pid, int chid)
{
    tm_channel_t *c = channel_find(pid, chid);
    return c ? (int)(c - g_channels) : -1;
}

int tm_connection_register_existing(pid_t client_pid, seL4_CPtr client_slot,
                                    int channel_idx, seL4_Word badge,
                                    unsigned flags)
{
    if (channel_idx < 0 || channel_idx >= TM_MAX_CHANNELS) return -EINVAL;
    if (!g_channels[channel_idx].in_use) return -EINVAL;
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        if (g_connections[i].in_use) continue;
        g_connections[i].in_use      = 1;
        g_connections[i].channel_idx = channel_idx;
        g_connections[i].badge       = badge;
        g_connections[i].client_pid  = client_pid;
        g_connections[i].client_slot = client_slot;
        g_connections[i].flags       = flags;
        return 0;
    }
    return -ENOMEM;
}

/* Find a connection by (client_pid, client_slot). Returns 0 if not
 * found. Used by ConnectServerInfo and ConnectFlags. */
static tm_connection_t *
connection_find_by_slot(pid_t client_pid, seL4_CPtr client_slot)
{
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        tm_connection_t *cn = &g_connections[i];
        if (cn->in_use &&
            cn->client_pid  == client_pid &&
            cn->client_slot == client_slot) {
            return cn;
        }
    }
    return 0;
}

/* Find a connection by badge (server-side view). scoid == badge in
 * v0.3.3. Used by ConnectClientInfo. */
static tm_connection_t *
connection_find_by_badge(seL4_Word badge)
{
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        tm_connection_t *cn = &g_connections[i];
        if (cn->in_use && cn->badge == badge) return cn;
    }
    return 0;
}

static int channel_alloc_slot_idx(void)
{
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (!g_channels[i].in_use) return i;
    }
    return -1;
}

static int connection_alloc_slot_idx(void)
{
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        if (!g_connections[i].in_use) return i;
    }
    return -1;
}

int tm_channel_create(pid_t owner_pid, int chid, unsigned flags,
                      seL4_CPtr *out_recv_slot)
{
    tm_process_t *owner = tm_process_lookup(owner_pid);
    if (!owner) return -ESRCH;
    if (channel_find(owner_pid, chid)) return -EINVAL;
    int idx = channel_alloc_slot_idx();
    if (idx < 0) return -ENOMEM;

    seL4_CPtr master = taskman_alloc_and_retype(seL4_EndpointObject, 0);
    if (!master) return -ENOMEM;

    /* Recv slot goes into the OWNER's CSpace. */
    seL4_CPtr recv = tm_process_alloc_slot(owner_pid);
    seL4_Uint8 dest_depth = cnode_depth_for(owner_pid);

    if (qsoe_cnode_copy(owner->cnode, recv, dest_depth,
                        s_cnode_root, master, TM_DEPTH_TASKMAN,
                        QSOE_RIGHTS_ALL) != 0) {
        qsoe_cnode_delete(s_cnode_root, master, TM_DEPTH_TASKMAN);
        return -ENOMEM;
    }

    g_channels[idx].in_use     = 1;
    g_channels[idx].master     = master;
    g_channels[idx].owner_recv = recv;
    g_channels[idx].owner_pid  = owner_pid;
    g_channels[idx].owner_chid = chid;
    g_channels[idx].flags      = flags;

    *out_recv_slot = recv;
    return 0;
}

int tm_channel_destroy(pid_t owner_pid, seL4_CPtr recv_slot)
{
    tm_channel_t *c = 0;
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (g_channels[i].in_use &&
            g_channels[i].owner_pid  == owner_pid &&
            g_channels[i].owner_recv == recv_slot) {
            c = &g_channels[i];
            break;
        }
    }
    if (!c) return -EBADF;

    /* Revoke from the master (cascades to recv cap + every Send-cap). */
    if (qsoe_cnode_revoke(s_cnode_root, c->master, TM_DEPTH_TASKMAN) != 0) {
        return -EBADF;
    }
    qsoe_cnode_delete(s_cnode_root, c->master, TM_DEPTH_TASKMAN);

    /* Forget any connections that targeted this channel. */
    for (int j = 0; j < TM_MAX_CONNECTIONS; ++j) {
        tm_connection_t *cn = &g_connections[j];
        if (cn->in_use && cn->channel_idx == (int)(c - g_channels)) {
            cn->in_use = 0;
        }
    }
    c->in_use = 0;
    return 0;
}

int tm_connect_attach(pid_t client_pid, pid_t target_pid, int target_chid,
                      unsigned flags, seL4_CPtr *out_send_slot)
{
    tm_process_t *client = tm_process_lookup(client_pid);
    if (!client) return -ESRCH;
    tm_channel_t *c = channel_find(target_pid, target_chid);
    if (!c) return -ESRCH;

    int cidx = connection_alloc_slot_idx();
    if (cidx < 0) return -ENOMEM;

    /* Send-cap slot goes into the CLIENT's CSpace. */
    seL4_CPtr send_slot = tm_process_alloc_slot(client_pid);
    seL4_Uint8 dest_depth = cnode_depth_for(client_pid);
    seL4_Word badge = s_next_badge++;

    if (qsoe_cnode_mint(client->cnode, send_slot, dest_depth,
                        s_cnode_root, c->master, TM_DEPTH_TASKMAN,
                        QSOE_RIGHTS_SEND, badge) != 0) {
        return -ENOMEM;
    }

    /* Keep only the per-connection (COF_*) bits; the namespace-selector
     * QSOE_SIDE_CHANNEL bit is for libqsoe's pool routing, not state. */
    g_connections[cidx].in_use      = 1;
    g_connections[cidx].channel_idx = (int)(c - g_channels);
    g_connections[cidx].badge       = badge;
    g_connections[cidx].client_pid  = client_pid;
    g_connections[cidx].client_slot = send_slot;
    g_connections[cidx].flags       = flags & ~QSOE_SIDE_CHANNEL;

    *out_send_slot = send_slot;
    return 0;
}

int tm_connect_detach(pid_t client_pid, seL4_CPtr send_slot)
{
    tm_process_t *client = tm_process_lookup(client_pid);
    if (!client) return -ESRCH;
    tm_connection_t *cn = 0;
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        if (g_connections[i].in_use &&
            g_connections[i].client_pid  == client_pid &&
            g_connections[i].client_slot == send_slot) {
            cn = &g_connections[i];
            break;
        }
    }
    if (!cn) return -EBADF;

    if (qsoe_cnode_delete(client->cnode, send_slot,
                          cnode_depth_for(client_pid)) != 0) {
        return -EBADF;
    }
    cn->in_use = 0;
    return 0;
}

/* ----------- v0.3.3 introspection handlers ----------- */

int tm_connect_server_info(pid_t caller_pid, seL4_CPtr client_slot,
                           pid_t *out_server_pid, int *out_server_chid,
                           seL4_Word *out_scoid)
{
    tm_connection_t *cn = connection_find_by_slot(caller_pid, client_slot);
    if (!cn) return -EBADF;
    tm_channel_t *ch = &g_channels[cn->channel_idx];
    *out_server_pid  = ch->owner_pid;
    *out_server_chid = ch->owner_chid;
    *out_scoid       = cn->badge;
    return 0;
}

int tm_connect_client_info(seL4_Word scoid,
                           pid_t *out_client_pid, pid_t *out_sid,
                           unsigned *out_flags)
{
    tm_connection_t *cn = connection_find_by_badge(scoid);
    if (!cn) return -EBADF;
    *out_client_pid = cn->client_pid;
    *out_sid        = cn->client_pid;  /* no sessions yet; sid := pid */
    *out_flags      = cn->flags;
    return 0;
}

int tm_connect_flags(pid_t caller_pid, seL4_CPtr client_slot,
                     unsigned mask, unsigned bits,
                     unsigned *out_old)
{
    tm_connection_t *cn = connection_find_by_slot(caller_pid, client_slot);
    if (!cn) return -EBADF;
    unsigned old = cn->flags;
    if (mask != 0) {
        cn->flags = (old & ~mask) | (bits & mask);
    }
    *out_old = old;
    return 0;
}
