/*
 * proc/connect.c — connection registry: ConnectAttach / ConnectDetach,
 * ConnectServerInfo / ConnectClientInfo / ConnectFlags, plus the
 * per-connection ctx and badge-by-slot helpers cpiofs and the IO
 * dispatcher rely on.
 *
 * Split out of v0.6.4's server.c.
 */

#include "proc.h"
#include "../qsoe_invoke.h"

static tm_connection_t g_connections[TM_MAX_CONNECTIONS];

/* channel.c imports this when destroying a channel. */
void tm_connections_drop_for_channel_idx(int channel_idx)
{
    for (int j = 0; j < TM_MAX_CONNECTIONS; ++j) {
        tm_connection_t *cn = &g_connections[j];
        if (cn->in_use && cn->channel_idx == channel_idx) {
            cn->in_use = 0;
        }
    }
}

/* process.c imports this during ProcessTerminate. */
void tm_connections_drop_for_pid(pid_t pid)
{
    for (int j = 0; j < TM_MAX_CONNECTIONS; ++j) {
        tm_connection_t *cn = &g_connections[j];
        if (cn->in_use && cn->client_pid == pid) {
            cn->in_use = 0;
        }
    }
}

static int connection_alloc_slot_idx(void)
{
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        if (!g_connections[i].in_use) return i;
    }
    return -1;
}

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

static tm_connection_t *
connection_find_by_badge(seL4_Word badge)
{
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        tm_connection_t *cn = &g_connections[i];
        if (cn->in_use && cn->badge == badge) return cn;
    }
    return 0;
}

int tm_connection_register_existing(pid_t client_pid, seL4_CPtr client_slot,
                                    int channel_idx, seL4_Word badge,
                                    unsigned flags)
{
    tm_channel_t *gchannels = tm_channels_array();
    if (channel_idx < 0 || channel_idx >= TM_MAX_CHANNELS) return -EINVAL;
    if (!gchannels[channel_idx].in_use) return -EINVAL;
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        if (g_connections[i].in_use) continue;
        g_connections[i].in_use      = 1;
        g_connections[i].channel_idx = channel_idx;
        g_connections[i].badge       = badge;
        g_connections[i].client_pid  = client_pid;
        g_connections[i].client_slot = client_slot;
        g_connections[i].ntfn_slot   = 0;
        g_connections[i].flags       = flags;
        g_connections[i].ctx[0]      = 0;
        g_connections[i].ctx[1]      = 0;
        return 0;
    }
    return -ENOMEM;
}

int tm_connection_clone_for_dup(pid_t client_pid, seL4_CPtr src_slot,
                                 seL4_CPtr dest_slot)
{
    tm_connection_t *src = connection_find_by_slot(client_pid, src_slot);
    if (!src) return -EBADF;
    int idx = connection_alloc_slot_idx();
    if (idx < 0) return -ENOMEM;
    g_connections[idx].in_use      = 1;
    g_connections[idx].channel_idx = src->channel_idx;
    g_connections[idx].badge       = src->badge;
    g_connections[idx].client_pid  = client_pid;
    g_connections[idx].client_slot = dest_slot;
    g_connections[idx].ntfn_slot   = 0;   /* dup falls back to the pulse path */
    g_connections[idx].flags       = src->flags;
    g_connections[idx].ctx[0]      = src->ctx[0];
    g_connections[idx].ctx[1]      = src->ctx[1];
    return 0;
}

int tm_connection_set_ctx(seL4_Word badge, unsigned long c0, unsigned long c1)
{
    tm_connection_t *cn = connection_find_by_badge(badge);
    if (!cn) return -ENOENT;
    cn->ctx[0] = c0;
    cn->ctx[1] = c1;
    return 0;
}

int tm_connection_get_ctx(seL4_Word badge, unsigned long *c0, unsigned long *c1)
{
    tm_connection_t *cn = connection_find_by_badge(badge);
    if (!cn) return -ENOENT;
    if (c0) *c0 = cn->ctx[0];
    if (c1) *c1 = cn->ctx[1];
    return 0;
}

int tm_connection_badge_by_slot(pid_t client_pid, seL4_CPtr slot,
                                seL4_Word *out_badge)
{
    tm_connection_t *cn = connection_find_by_slot(client_pid, slot);
    if (!cn) return -ENOENT;
    if (out_badge) *out_badge = cn->badge;
    return 0;
}

/* Resolve a connection badge (the scoid a server's MsgReceive sees) to
 * the client pid that owns the connection.  Backs bulk IPC, where the
 * server relays its receive badge so taskman can reach the blocked
 * sender's buffers.  Returns 0 if the badge names no live connection. */
pid_t tm_connection_client_pid(seL4_Word badge)
{
    tm_connection_t *cn = connection_find_by_badge(badge);
    return cn ? cn->client_pid : 0;
}

int tm_channel_by_badge(seL4_Word badge, pid_t *out_pid, int *out_chid)
{
    tm_connection_t *cn = connection_find_by_badge(badge);
    if (!cn) return -ENOENT;
    if (cn->channel_idx < 0 || cn->channel_idx >= TM_MAX_CHANNELS) return -ENOENT;
    tm_channel_t *gchannels = tm_channels_array();
    tm_channel_t *c = &gchannels[cn->channel_idx];
    if (!c->in_use) return -ENOENT;
    if (out_pid)  *out_pid  = c->owner_pid;
    if (out_chid) *out_chid = c->owner_chid;
    return 0;
}

int tm_connect_attach(pid_t client_pid, pid_t target_pid, int target_chid,
                      unsigned flags, seL4_CPtr *out_send_slot,
                      seL4_CPtr *out_ntfn_slot)
{
    *out_ntfn_slot = 0;
    tm_process_t *client = tm_process_lookup(client_pid);
    if (!client) return -ESRCH;
    int target_idx = tm_channel_index(target_pid, target_chid);
    if (target_idx < 0) return -ESRCH;
    tm_channel_t *gchannels = tm_channels_array();
    tm_channel_t *c = &gchannels[target_idx];

    int cidx = connection_alloc_slot_idx();
    if (cidx < 0) return -ENOMEM;

    seL4_CPtr send_slot = tm_process_alloc_slot(client_pid);
    seL4_Uint8 dest_depth = cnode_depth_for(client_pid);
    seL4_Word badge = tm_alloc_scoid();

    if (qsoe_cnode_mint(client->cnode, send_slot, dest_depth,
                        s_cnode_root, c->master, TM_DEPTH_TASKMAN,
                        QSOE_RIGHTS_SEND, badge) != 0) {
        return -ENOMEM;
    }

    /* QSOE_CHF_PULSE_DIRECT: also hand the client a copy of the channel's
     * pulse Notification Send-cap, so its MsgSendPulse signals the receiver
     * straight through the kernel instead of routing TM_REQ_PULSE_SEND back
     * here.  Lets a device wake its owner while taskman is blocked (e.g. in
     * a spawn-image read off that same device).  Best-effort: if the copy
     * fails we leave ntfn_slot 0 and the client falls back to the pulse
     * path -- correctness is unaffected, only the deadlock-freedom is. */
    seL4_CPtr ntfn_slot = 0;
    if ((c->flags & QSOE_CHF_PULSE_DIRECT) && c->ntfn_sig) {
        ntfn_slot = tm_process_alloc_slot(client_pid);
        if (qsoe_cnode_copy(client->cnode, ntfn_slot, dest_depth,
                            s_cnode_root, c->ntfn_sig, TM_DEPTH_TASKMAN,
                            QSOE_RIGHTS_SEND) != 0) {
            ntfn_slot = 0;   /* slot stays claimed; reclaimed at process exit */
        }
    }

    g_connections[cidx].in_use      = 1;
    g_connections[cidx].channel_idx = target_idx;
    g_connections[cidx].badge       = badge;
    g_connections[cidx].client_pid  = client_pid;
    g_connections[cidx].client_slot = send_slot;
    g_connections[cidx].ntfn_slot   = ntfn_slot;
    g_connections[cidx].flags       = flags & ~QSOE_SIDE_CHANNEL;

    *out_send_slot = send_slot;
    *out_ntfn_slot = ntfn_slot;
    return 0;
}

int tm_connect_detach(pid_t client_pid, seL4_CPtr send_slot)
{
    tm_process_t *client = tm_process_lookup(client_pid);
    if (!client) return -ESRCH;
    tm_connection_t *cn = connection_find_by_slot(client_pid, send_slot);
    if (!cn) return -EBADF;

    if (qsoe_cnode_delete(client->cnode, send_slot,
                          cnode_depth_for(client_pid)) != 0) {
        return -EBADF;
    }
    /* Drop the direct-pulse Notification copy too, if this was a
     * QSOE_CHF_PULSE_DIRECT connection. */
    if (cn->ntfn_slot) {
        (void)qsoe_cnode_delete(client->cnode, cn->ntfn_slot,
                                cnode_depth_for(client_pid));
        cn->ntfn_slot = 0;
    }
    cn->in_use = 0;
    return 0;
}

int tm_connect_server_info(pid_t caller_pid, seL4_CPtr client_slot,
                           pid_t *out_server_pid, int *out_server_chid,
                           seL4_Word *out_scoid)
{
    tm_connection_t *cn = connection_find_by_slot(caller_pid, client_slot);
    if (!cn) return -EBADF;
    tm_channel_t *gchannels = tm_channels_array();
    tm_channel_t *ch = &gchannels[cn->channel_idx];
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
    *out_sid        = cn->client_pid;
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
