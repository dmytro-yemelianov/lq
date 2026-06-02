/*
 * proc/channel.c — channel registry: ChannelCreate / ChannelDestroy /
 * register-existing, plus the badge allocator that scoid values come
 * from.  Backs TM_REQ_CHANNEL_{CREATE,DESTROY}.
 *
 * Split out of v0.6.4's server.c.
 */

#include "proc.h"
#include "../qsoe_invoke.h"

static tm_channel_t g_channels[TM_MAX_CHANNELS];
static seL4_Word    s_next_badge = 1;  /* 0 reserved as "no badge" */

tm_channel_t *tm_channels_array(void) { return g_channels; }

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
    g_channels[idx].in_use      = 1;
    g_channels[idx].master      = master_slot;
    g_channels[idx].owner_recv  = recv_slot;
    g_channels[idx].owner_pid   = pid;
    g_channels[idx].owner_chid  = chid;
    g_channels[idx].flags       = 0;
    g_channels[idx].pulse_head  = 0;
    g_channels[idx].pulse_tail  = 0;
    g_channels[idx].pulse_count = 0;
    g_channels[idx].ntfn_master = 0;
    g_channels[idx].ntfn_sig    = 0;
    return 0;
}

int tm_channel_index(pid_t pid, int chid)
{
    tm_channel_t *c = channel_find(pid, chid);
    return c ? (int)(c - g_channels) : -1;
}

seL4_CPtr tm_channel_master(int idx)
{
    if (idx < 0 || idx >= TM_MAX_CHANNELS) return 0;
    if (!g_channels[idx].in_use) return 0;
    return g_channels[idx].master;
}

seL4_Word tm_alloc_scoid(void)
{
    return s_next_badge++;
}

static int channel_alloc_slot_idx(void)
{
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (!g_channels[i].in_use) return i;
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

    seL4_CPtr recv = tm_process_alloc_slot(owner_pid);
    seL4_Uint8 dest_depth = cnode_depth_for(owner_pid);

    if (qsoe_cnode_copy(owner->cnode, recv, dest_depth,
                        s_cnode_root, master, TM_DEPTH_TASKMAN,
                        QSOE_RIGHTS_ALL) != 0) {
        qsoe_cnode_delete(s_cnode_root, master, TM_DEPTH_TASKMAN);
        return -ENOMEM;
    }

    /* Per-channel Notification for pulse wake.  ntfn_master is
     * unbadged (used for Bind/Unbind/Revoke); ntfn_sig is a Send-cap
     * minted with badge=QSOE_NTFN_BADGE_BIT. */
    seL4_CPtr ntfn_master = taskman_alloc_and_retype(seL4_NotificationObject,
                                                      seL4_NotificationBits);
    seL4_CPtr ntfn_sig    = 0;
    if (ntfn_master) {
        ntfn_sig = taskman_alloc_empty_slot();
        seL4_CapRights_t sig_rights = seL4_CapRights_new(0, 0, 0, 1);
        if (qsoe_cnode_mint(s_cnode_root, ntfn_sig, TM_DEPTH_TASKMAN,
                            s_cnode_root, ntfn_master, TM_DEPTH_TASKMAN,
                            sig_rights, QSOE_NTFN_BADGE_BIT) != 0) {
            qsoe_cnode_delete(s_cnode_root, ntfn_master, TM_DEPTH_TASKMAN);
            taskman_free_slot(ntfn_master);
            taskman_free_slot(ntfn_sig);
            ntfn_master = ntfn_sig = 0;
        } else if (owner->tcb) {
            if (qsoe_tcb_bind_notification(owner->tcb, ntfn_master) != 0) {
                qsoe_cnode_revoke(s_cnode_root, ntfn_master, TM_DEPTH_TASKMAN);
                qsoe_cnode_delete(s_cnode_root, ntfn_master, TM_DEPTH_TASKMAN);
                taskman_free_slot(ntfn_master);
                taskman_free_slot(ntfn_sig);
                ntfn_master = ntfn_sig = 0;
            }
        }
    }

    g_channels[idx].in_use      = 1;
    g_channels[idx].master      = master;
    g_channels[idx].owner_recv  = recv;
    g_channels[idx].owner_pid   = owner_pid;
    g_channels[idx].owner_chid  = chid;
    g_channels[idx].flags       = flags;
    g_channels[idx].pulse_head  = 0;
    g_channels[idx].pulse_tail  = 0;
    g_channels[idx].pulse_count = 0;
    g_channels[idx].ntfn_master = ntfn_master;
    g_channels[idx].ntfn_sig    = ntfn_sig;

    *out_recv_slot = recv;
    return 0;
}

/* Forward decl — defined in connect.c.  Used here to invalidate
 * connections that targeted a channel about to be destroyed. */
void tm_connections_drop_for_channel_idx(int channel_idx);

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

    /* Unbind the per-channel Notification BEFORE revoking — otherwise
     * the kernel leaves a dangling binding. */
    if (c->ntfn_master) {
        tm_process_t *owner = tm_process_lookup(c->owner_pid);
        if (owner && owner->tcb) {
            qsoe_tcb_unbind_notification(owner->tcb);
        }
        qsoe_cnode_revoke(s_cnode_root, c->ntfn_master, TM_DEPTH_TASKMAN);
        qsoe_cnode_delete(s_cnode_root, c->ntfn_master, TM_DEPTH_TASKMAN);
        taskman_free_slot(c->ntfn_master);
        taskman_free_slot(c->ntfn_sig);
        c->ntfn_master = 0;
        c->ntfn_sig = 0;
    }

    /* Revoke the master (cascades to recv cap + every Send-cap). */
    if (qsoe_cnode_revoke(s_cnode_root, c->master, TM_DEPTH_TASKMAN) != 0) {
        return -EBADF;
    }
    qsoe_cnode_delete(s_cnode_root, c->master, TM_DEPTH_TASKMAN);
    taskman_free_slot(c->master);

    /* Forget any connections that targeted this channel. */
    tm_connections_drop_for_channel_idx((int)(c - g_channels));
    c->in_use = 0;
    return 0;
}
