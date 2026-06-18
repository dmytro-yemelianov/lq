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

/* System-wide counter for global-channel chids (QSOE_CHF_GLOBAL).  LQ
 * chids are otherwise allocated client-side per-process, which can't give
 * a global channel the cross-process-unique chid it needs; taskman assigns
 * it here.  Starts at 1 so a global chid is never bare QSOE_GLOBAL_CHANNEL. */
static unsigned     s_global_chid_next = 1;

tm_channel_t *tm_channels_array(void) { return g_channels; }

/* A global chid (QSOE_GLOBAL_CHANNEL bit set) is matched by chid ALONE --
 * pid is ignored, since the whole point is pid-independent discovery.  A
 * normal chid is matched by the (owner_pid, owner_chid) pair (the QNX
 * per-process model). */
static tm_channel_t *channel_find(pid_t pid, int chid)
{
    int global = QSOE_IS_GLOBAL_CHANNEL(chid) != 0;
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        tm_channel_t *c = &g_channels[i];
        if (!c->in_use || c->owner_chid != chid) continue;
        if (global || c->owner_pid == pid) return c;
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
                      int creator_tid, seL4_CPtr *out_recv_slot, int *out_chid)
{
    tm_process_t *owner = tm_process_lookup(owner_pid);
    if (!owner) return -ESRCH;

    /* The pulse Notification (below) is bound to the thread that will
     * RECEIVE on this channel.  By convention that's the creating thread:
     * a server thread MsgReceives its own channel (e.g. devb-nvme's IST
     * creates AND receives the MSI pulse channel), so bind to the creator.
     * tid<=1 is the main thread (owner->tcb); a worker resolves through
     * tm_thread_find.  When receiver != creator (the signal channel, made
     * at startup but serviced by the system thread) a later
     * TM_REQ_CHANNEL_BIND_THREAD re-targets it.  seL4 binds one ntfn per
     * TCB, so this is "one pulse-receiving channel per thread". */
    seL4_CPtr bind_tcb = owner->tcb;
    if (creator_tid > 1) {
        tm_thread_t *t = tm_thread_find(owner_pid, creator_tid);
        if (t) bind_tcb = t->tcb_master;
    }
    /* Global channel: taskman assigns a system-unique chid (the client's
     * suggestion is ignored) and marks it so ConnectAttach/MsgReceive route
     * via the global path.  owner_pid is still recorded so the channel is
     * torn down with the creating process. */
    if (flags & QSOE_CHF_GLOBAL)
        chid = (int)(QSOE_GLOBAL_CHANNEL | (s_global_chid_next++));
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
        } else if (bind_tcb) {
            if (qsoe_tcb_bind_notification(bind_tcb, ntfn_master) != 0) {
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
    if (out_chid) *out_chid = chid;   /* effective chid (assigned if global) */
    return 0;
}

/* TM_REQ_CHANNEL_BIND_THREAD: move chid's pulse Notification from the
 * main TCB (where tm_channel_create bound it) onto the process's system
 * thread `tid`, so that thread -- not main -- wakes on kill() pulses.
 * seL4 binds at most one Notification per TCB, and the signal channel
 * is the first channel a process creates (in __qsoe_syschan_init, before
 * main), so the main TCB's current binding IS this channel's: unbind it
 * cleanly, then bind to the system thread.  The "sigthread" ps(1) -H
 * label is no longer set here: the system thread names itself through
 * ThreadCtl(TCTL_NAME) -> TM_REQ_THREAD_SETNAME, the same path every
 * other thread uses. */
int tm_channel_bind_thread(pid_t owner_pid, int chid, int tid)
{
    tm_process_t *owner = tm_process_lookup(owner_pid);
    if (!owner) return -ESRCH;
    tm_channel_t *c = channel_find(owner_pid, chid);
    if (!c) return -EINVAL;
    if (!c->ntfn_master) return -EINVAL;   /* channel has no pulse ntfn */

    tm_thread_t *t = tm_thread_find(owner_pid, tid);
    if (!t || !t->tcb_master) return -ESRCH;

    /* Drop the default main-TCB binding, then attach to the system
     * thread.  Unbind is harmless if main wasn't bound. seL4 allows at
     * most one bound Notification per TCB, so the unbind must precede
     * the bind. */
    if (owner->tcb) (void)qsoe_tcb_unbind_notification(owner->tcb);
    if (qsoe_tcb_bind_notification(t->tcb_master, c->ntfn_master) != 0)
        return -EINVAL;

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
