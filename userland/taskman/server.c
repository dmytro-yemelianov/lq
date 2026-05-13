/*
 * server.c — taskman's channel / connection registries and the four
 * handler functions invoked by libqsoe.
 *
 * v0.x simplifications:
 *   - Flat arrays with linear scan for both registries.
 *   - Bump slot allocator over the caller's CNode (no recycling).
 *   - Single backing untyped (the largest non-device one).
 *   - Single-threaded; no locking.
 *
 * See Design doc §3.4 and §4.3.
 */

#include "server.h"
#include "qsoe_invoke.h"

static tm_channel_t    g_channels[TM_MAX_CHANNELS];
static tm_connection_t g_connections[TM_MAX_CONNECTIONS];

/* Shared with spawn.c — kept non-static for v0.3.0 simplicity; v0.4
 * wraps these in a proper accessor API. */
seL4_CPtr s_untyped;
seL4_CPtr s_cnode_root;
seL4_CPtr s_next_slot;
static seL4_Word s_next_badge = 1; /* 0 reserved as "no badge" */

void tm_init(seL4_CPtr ut, seL4_CPtr cnode_root, seL4_CPtr first_free)
{
    s_untyped    = ut;
    s_cnode_root = cnode_root;
    s_next_slot  = first_free;
}

static seL4_CPtr alloc_slot(void)
{
    return s_next_slot++;
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
    if (channel_find(owner_pid, chid)) return -EINVAL;
    int idx = channel_alloc_slot_idx();
    if (idx < 0) return -ENOMEM;

    seL4_CPtr master = alloc_slot();
    seL4_CPtr recv   = alloc_slot();

    /* 1. Retype the master endpoint out of the backing untyped. */
    if (qsoe_untyped_retype(s_untyped, seL4_EndpointObject, 0,
                            s_cnode_root, 0, 0, master, 1) != 0) {
        return -ENOMEM;
    }
    /* 2. Copy master → recv with full rights (the server's "Recv" view). */
    if (qsoe_cnode_copy(s_cnode_root, recv, 64,
                        s_cnode_root, master, 64,
                        QSOE_RIGHTS_ALL) != 0) {
        qsoe_cnode_delete(s_cnode_root, master, 64);
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

    if (qsoe_cnode_revoke(s_cnode_root, c->master, 64) != 0) {
        return -EBADF;
    }
    qsoe_cnode_delete(s_cnode_root, c->master, 64);

    /* Tear down any matching connections (the revoke already deleted
     * the kernel caps; we just clear our ledger entries). */
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
    (void)flags;
    tm_channel_t *c = channel_find(target_pid, target_chid);
    if (!c) return -ESRCH;

    int cidx = connection_alloc_slot_idx();
    if (cidx < 0) return -ENOMEM;

    seL4_CPtr send_slot = alloc_slot();
    seL4_Word badge = s_next_badge++;

    if (qsoe_cnode_mint(s_cnode_root, send_slot, 64,
                        s_cnode_root, c->master, 64,
                        QSOE_RIGHTS_SEND, badge) != 0) {
        return -ENOMEM;
    }

    g_connections[cidx].in_use      = 1;
    g_connections[cidx].channel_idx = (int)(c - g_channels);
    g_connections[cidx].badge       = badge;
    g_connections[cidx].client_pid  = client_pid;
    g_connections[cidx].client_slot = send_slot;

    *out_send_slot = send_slot;
    return 0;
}

int tm_connect_detach(pid_t client_pid, seL4_CPtr send_slot)
{
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

    if (qsoe_cnode_delete(s_cnode_root, send_slot, 64) != 0) {
        return -EBADF;
    }
    cn->in_use = 0;
    return 0;
}
