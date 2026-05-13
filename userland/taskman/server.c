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
#include "spawn.h"
#include "../libqsoe/include/qsoe/slots.h"
#include <cpio/cpio.h>

static tm_channel_t    g_channels[TM_MAX_CHANNELS];
static tm_connection_t g_connections[TM_MAX_CONNECTIONS];
static tm_process_t    g_processes[TM_MAX_PROCESSES];
static tm_thread_t     g_threads[TM_MAX_THREADS];

/* Shared with spawn.c — kept non-static for v0.3.0 simplicity; v0.4.1
 * wraps these in a proper accessor API. */
seL4_CPtr s_untyped;
seL4_CPtr s_cnode_root;
seL4_CPtr s_next_slot;
static seL4_Word s_next_badge = 1; /* 0 reserved as "no badge" */

/* v0.4.1 pid allocator. pid 1 is reserved for taskman forever; child
 * pids start at 2 and are bump-allocated up to TM_MAX_PROCESSES-1.
 * Freed pids land on a free list and are reused (LIFO) before the
 * bump pointer advances. */
static pid_t s_next_pid          = 2;
static pid_t s_pid_free_list[TM_MAX_PROCESSES];
static int   s_pid_free_count    = 0;

pid_t tm_pid_alloc(void)
{
    if (s_pid_free_count > 0) {
        return s_pid_free_list[--s_pid_free_count];
    }
    if (s_next_pid >= TM_MAX_PROCESSES) return 0;
    return s_next_pid++;
}

void tm_pid_free(pid_t pid)
{
    if (pid <= QSOE_PID_TASKMAN) return;  /* never recycle taskman's pid */
    if (s_pid_free_count < TM_MAX_PROCESSES) {
        s_pid_free_list[s_pid_free_count++] = pid;
    }
}

/* v0.4.1: handles for the embedded userland CPIO and taskman's
 * primary endpoint, set by main() after boot. ProcessCreate uses
 * them to locate ELFs and badge SYSMGR caps into new children. */
static const void *s_cpio_start;
static unsigned long s_cpio_len;
static seL4_CPtr   s_primary_ep;

void tm_set_userland_cpio(const void *start, unsigned long len)
{
    s_cpio_start = start;
    s_cpio_len   = len;
}

void tm_set_primary_ep(seL4_CPtr ep)
{
    s_primary_ep = ep;
}

int tm_process_create_by_name(const char *path, unsigned path_len,
                              pid_t *out_pid)
{
    if (path_len == 0 || path_len >= 64) return -EINVAL;
    if (!s_cpio_start || !s_primary_ep) return -EINVAL;

    /* Copy and NUL-terminate — libcpio's lookup wants a C string. */
    char name[64];
    for (unsigned i = 0; i < path_len; ++i) name[i] = path[i];
    name[path_len] = 0;

    unsigned long elf_size = 0;
    const void *elf = cpio_get_file(s_cpio_start, s_cpio_len, name, &elf_size);
    if (!elf) return -ENOENT;

    pid_t new_pid = tm_pid_alloc();
    if (!new_pid) return -ENOMEM;

    int sr = tm_spawn(elf, elf_size, new_pid, s_primary_ep);
    if (sr) {
        tm_pid_free(new_pid);
        return sr;
    }
    *out_pid = new_pid;
    return 0;
}

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
    g_processes[0].in_use         = 1;
    g_processes[0].pid            = QSOE_PID_TASKMAN;
    g_processes[0].cnode          = cnode_root;
    g_processes[0].next_slot      = first_free;
    g_processes[0].tcb            = seL4_CapInitThreadTCB;
    g_processes[0].vspace         = seL4_CapInitThreadVSpace;
    g_processes[0].untyped_budget = 0;  /* taskman has the big root untyped */
    g_processes[0].workers_l1_pt  = 0;
    g_processes[0].workers_l0_pt  = 0;
    g_processes[0].next_tid       = 2;
}

int tm_process_register(pid_t pid, seL4_CPtr cnode,
                        seL4_CPtr tcb, seL4_CPtr vspace,
                        seL4_CPtr first_free_slot)
{
    if (tm_process_lookup(pid)) return -EINVAL;
    for (int i = 0; i < TM_MAX_PROCESSES; ++i) {
        if (g_processes[i].in_use) continue;
        g_processes[i].in_use         = 1;
        g_processes[i].pid            = pid;
        g_processes[i].cnode          = cnode;
        g_processes[i].next_slot      = first_free_slot;
        g_processes[i].tcb            = tcb;
        g_processes[i].vspace         = vspace;
        g_processes[i].untyped_budget = 0;  /* filled by spawn.c */
        g_processes[i].workers_l1_pt  = 0;
        g_processes[i].workers_l0_pt  = 0;
        g_processes[i].next_tid       = 2;
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

/* v0.4.1: free list of slots that were CNode_Deleted on Destroy paths,
 * so subsequent allocations reuse them instead of bumping s_next_slot
 * forever. Required for the cap-leak smoke test to show 0 growth. */
#define TM_SLOT_FREE_LIST_MAX 256
static seL4_CPtr s_slot_free_list[TM_SLOT_FREE_LIST_MAX];
static int       s_slot_free_count = 0;

static void taskman_free_slot(seL4_CPtr slot)
{
    if (slot == 0) return;
    if (s_slot_free_count < TM_SLOT_FREE_LIST_MAX) {
        s_slot_free_list[s_slot_free_count++] = slot;
    }
    /* If the free list is full we just drop it — the slot stays
     * unused, which costs CSpace room but never corrupts state. */
}

/* Allocate one untyped retype into a slot of taskman's CSpace. Reuses
 * a freed slot if available; otherwise bumps s_next_slot. Master caps
 * always live in taskman's CSpace regardless of who created the
 * channel. */
static seL4_CPtr taskman_alloc_and_retype(seL4_Word type, seL4_Word size_bits)
{
    seL4_CPtr slot;
    if (s_slot_free_count > 0) {
        slot = s_slot_free_list[--s_slot_free_count];
    } else {
        slot = s_next_slot++;
    }
    seL4_Word err = qsoe_untyped_retype(s_untyped, type, size_bits,
                                         s_cnode_root, 0, 0, slot, 1);
    if (err != 0) {
        /* Retype failed — give the slot back. */
        taskman_free_slot(slot);
        return 0;
    }
    return slot;
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
    g_channels[idx].pulse_head = 0;
    g_channels[idx].pulse_tail = 0;
    g_channels[idx].pulse_count = 0;
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
    g_channels[idx].pulse_head = 0;
    g_channels[idx].pulse_tail = 0;
    g_channels[idx].pulse_count = 0;

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
    taskman_free_slot(c->master);

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

/* ----------- v0.4.1 process termination ----------- */

/* tm_process_terminate — revoke a process's master caps in this order:
 *   1. worker thread TCBs and join Notifications (this pid's entries
 *      in g_threads[]) — stops every thread of the process
 *   2. channels owned by the process — cascade-revokes Send caps
 *      that other processes held to this server, breaking their
 *      connections cleanly
 *   3. connections this process held as a client — just remove from
 *      registry; the Send caps live in the process's CSpace which
 *      goes away when its CNode is revoked
 *   4. main TCB — done after workers so the main thread is the last
 *      to be inactivated (for self-terminate, this revokes the
 *      caller's own TCB and the dispatch loop must skip the reply)
 *   5. VSpace — kernel walks and unmaps all mapped frames
 *   6. CNode — frees every cap the process held
 *   7. workers_l0_pt if allocated
 *
 * v0.4.1 does NOT explicitly free the page-tables retyped by spawn.c
 * (L1, image L0, IPC/stack frames) — they sit retyped in taskman's
 * CSpace until process-table compaction in v0.5+. The slot bump
 * pointer (s_next_slot) keeps growing per ProcessCreate/Terminate
 * cycle; that's documented in the cap-leak test.
 */
int tm_process_terminate(pid_t target, int status)
{
    (void)status;  /* not propagated to waiters until v0.5's waitpid */
    if (target == QSOE_PID_TASKMAN) return -EINVAL;
    tm_process_t *p = tm_process_lookup(target);
    if (!p) return -ESRCH;

    /* 1. Worker threads of this pid. */
    for (int i = 0; i < TM_MAX_THREADS; ++i) {
        if (g_threads[i].in_use && g_threads[i].pid == target) {
            qsoe_cnode_revoke(s_cnode_root, g_threads[i].tcb_master, TM_DEPTH_TASKMAN);
            qsoe_cnode_delete(s_cnode_root, g_threads[i].tcb_master, TM_DEPTH_TASKMAN);
            qsoe_cnode_revoke(s_cnode_root, g_threads[i].ntfn_master, TM_DEPTH_TASKMAN);
            qsoe_cnode_delete(s_cnode_root, g_threads[i].ntfn_master, TM_DEPTH_TASKMAN);
            g_threads[i].in_use = 0;
        }
    }

    /* 2. Channels owned by this pid. */
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (g_channels[i].in_use && g_channels[i].owner_pid == target) {
            qsoe_cnode_revoke(s_cnode_root, g_channels[i].master, TM_DEPTH_TASKMAN);
            qsoe_cnode_delete(s_cnode_root, g_channels[i].master, TM_DEPTH_TASKMAN);
            g_channels[i].in_use = 0;
        }
    }

    /* 3. Connections this pid held as a client — mark dead; the cap
     *    in the client's CSpace will vanish when the CNode is revoked. */
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        if (g_connections[i].in_use && g_connections[i].client_pid == target) {
            g_connections[i].in_use = 0;
        }
    }

    /* 4. Main TCB. */
    qsoe_cnode_revoke(s_cnode_root, p->tcb, TM_DEPTH_TASKMAN);
    qsoe_cnode_delete(s_cnode_root, p->tcb, TM_DEPTH_TASKMAN);

    /* 5. VSpace — kernel unmaps everything walking the table tree. */
    qsoe_cnode_revoke(s_cnode_root, p->vspace, TM_DEPTH_TASKMAN);
    qsoe_cnode_delete(s_cnode_root, p->vspace, TM_DEPTH_TASKMAN);

    /* 6. CNode — all of the process's slot-1+ caps go with it. */
    qsoe_cnode_revoke(s_cnode_root, p->cnode, TM_DEPTH_TASKMAN);
    qsoe_cnode_delete(s_cnode_root, p->cnode, TM_DEPTH_TASKMAN);

    /* 7. Workers L0/L1 PTs if allocated. */
    if (p->workers_l0_pt) {
        qsoe_cnode_delete(s_cnode_root, p->workers_l0_pt, TM_DEPTH_TASKMAN);
    }
    if (p->workers_l1_pt) {
        qsoe_cnode_delete(s_cnode_root, p->workers_l1_pt, TM_DEPTH_TASKMAN);
    }

    /* 8. Untyped budget. Revoke first to free derived caps inside it,
     *    then delete the master. */
    if (p->untyped_budget) {
        qsoe_cnode_revoke(s_cnode_root, p->untyped_budget, TM_DEPTH_TASKMAN);
        qsoe_cnode_delete(s_cnode_root, p->untyped_budget, TM_DEPTH_TASKMAN);
    }

    p->in_use = 0;
    tm_pid_free(target);
    return 0;
}

/* ----------- v0.4 thread allocator ----------- */

static int thread_alloc_slot_idx(void)
{
    for (int i = 0; i < TM_MAX_THREADS; ++i) {
        if (!g_threads[i].in_use) return i;
    }
    return -1;
}

/* Ensure the caller's VSpace has page tables for the worker thread
 * region at [0x40000000, 0x40200000) — a separate 1 GiB L1 region
 * from the image's L1 (which covers [0, 1 GiB)). Per
 * [[project-image-size-cap]] the image stays ≤ 1 GiB so the worker
 * region and the image region never share an L1 PT.
 *
 * Sv39 walk: vaddr 0x40000000 has L2-index=1, L1-index=0. First
 * PageTable_Map call lands at L2 slot 1 (creating an L1 PT covering
 * [0x40000000, 0x80000000)); second call lands at L1 slot 0 within
 * that new L1 (creating an L0 PT covering [0x40000000, 0x40200000)).
 * Both are allocated once per process on the first ThreadCreate. */
static int ensure_workers_pts(tm_process_t *p)
{
    if (p->workers_l0_pt) return 0;
    if (!p->workers_l1_pt) {
        seL4_CPtr l1 = taskman_alloc_and_retype(seL4_RISCV_PageTableObject, 0);
        if (!l1) return -ENOMEM;
        seL4_Word err = qsoe_riscv_pagetable_map(l1, p->vspace,
                                                  0x40000000UL,
                                                  QSOE_VM_ATTR_DEFAULT);
        if (err) return -ENOMEM;
        p->workers_l1_pt = l1;
    }
    seL4_CPtr l0 = taskman_alloc_and_retype(seL4_RISCV_PageTableObject, 0);
    if (!l0) return -ENOMEM;
    seL4_Word err = qsoe_riscv_pagetable_map(l0, p->vspace,
                                              0x40000000UL,
                                              QSOE_VM_ATTR_DEFAULT);
    if (err) return -ENOMEM;
    p->workers_l0_pt = l0;
    return 0;
}

int tm_thread_alloc(pid_t caller_pid,
                    unsigned long stack_top_vaddr, unsigned stack_pages,
                    unsigned long ipc_vaddr,
                    unsigned prio, unsigned affinity,
                    int *out_tid,
                    seL4_CPtr *out_tcb_slot,
                    seL4_CPtr *out_ntfn_slot)
{
    if (caller_pid == QSOE_PID_TASKMAN) {
        /* taskman is single-threaded in v0.4; ThreadCreate inside
         * taskman is unimplemented. */
        return -ENOSYS;
    }
    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;
    if (p->next_tid >= TM_MAX_TID_PER_PROC + 1) return -ENOMEM;
    if (stack_pages == 0 || stack_pages > 16) return -EINVAL;

    int gidx = thread_alloc_slot_idx();
    if (gidx < 0) return -ENOMEM;

    int new_tid = p->next_tid++;

    /* Make sure the page-table tree covers [0x200000, 0x400000). */
    int pterr = ensure_workers_pts(p);
    if (pterr) return pterr;

    /* Allocate kernel objects (master caps in taskman's CSpace). */
    seL4_CPtr tcb       = taskman_alloc_and_retype(seL4_TCBObject, 0);
    if (!tcb) return -ENOMEM;
    seL4_CPtr ntfn      = taskman_alloc_and_retype(seL4_NotificationObject, 0);
    if (!ntfn) return -ENOMEM;
    seL4_CPtr ipc_frame = taskman_alloc_and_retype(seL4_RISCV_4K_Page, 0);
    if (!ipc_frame) return -ENOMEM;

    /* Map IPC buffer into caller's VSpace. */
    seL4_Word err = qsoe_riscv_page_map(ipc_frame, p->vspace, ipc_vaddr,
                                         QSOE_RIGHTS_ALL,
                                         QSOE_VM_ATTR_DEFAULT);
    if (err) return -ENOMEM;

    /* Map stack frames. Stack grows down — frame i covers vaddr
     * [stack_top - (i+1)*4K, stack_top - i*4K). The frames are leaked
     * on destroy in v0.4 (v0.4.1+ records them for cleanup). */
    for (unsigned i = 0; i < stack_pages; ++i) {
        seL4_CPtr f = taskman_alloc_and_retype(seL4_RISCV_4K_Page, 0);
        if (!f) return -ENOMEM;
        unsigned long va = stack_top_vaddr - (unsigned long)(i + 1) * 4096UL;
        err = qsoe_riscv_page_map(f, p->vspace, va,
                                   QSOE_RIGHTS_ALL,
                                   QSOE_VM_ATTR_DEFAULT);
        if (err) return -ENOMEM;
    }

    /* Configure the TCB. The new thread shares the caller's CSpace
     * (guard 52, 12-bit radix) and VSpace, exactly as the caller's
     * main thread was configured at spawn time. */
    err = qsoe_tcb_configure(tcb, 0 /*fault_ep*/,
                              p->cnode, 52UL,
                              p->vspace, 0,
                              ipc_vaddr, ipc_frame);
    if (err) return -ENOMEM;

    err = qsoe_tcb_set_priority(tcb, seL4_CapInitThreadTCB, prio);
    if (err) return -ENOMEM;

    /* Pin the new TCB to the requested CPU. seL4 defaults new TCBs to
     * CPU 0; without SetAffinity every worker piles onto hart 0. */
    err = qsoe_tcb_set_affinity(tcb, affinity);
    if (err) return -ENOMEM;

    /* Copy TCB and Notification caps into the caller's CSpace so the
     * caller can WriteRegisters / Resume / Signal / Wait on them. */
    seL4_CPtr child_tcb_slot  = tm_process_alloc_slot(caller_pid);
    seL4_CPtr child_ntfn_slot = tm_process_alloc_slot(caller_pid);
    seL4_Uint8 ddepth = cnode_depth_for(caller_pid);

    if (qsoe_cnode_copy(p->cnode, child_tcb_slot, ddepth,
                        s_cnode_root, tcb, TM_DEPTH_TASKMAN,
                        QSOE_RIGHTS_ALL) != 0) return -ENOMEM;
    if (qsoe_cnode_copy(p->cnode, child_ntfn_slot, ddepth,
                        s_cnode_root, ntfn, TM_DEPTH_TASKMAN,
                        QSOE_RIGHTS_ALL) != 0) return -ENOMEM;

    g_threads[gidx].in_use         = 1;
    g_threads[gidx].pid            = caller_pid;
    g_threads[gidx].tid            = new_tid;
    g_threads[gidx].tcb_master     = tcb;
    g_threads[gidx].ntfn_master    = ntfn;
    g_threads[gidx].tcb_in_caller  = child_tcb_slot;
    g_threads[gidx].ntfn_in_caller = child_ntfn_slot;

    *out_tid       = new_tid;
    *out_tcb_slot  = child_tcb_slot;
    *out_ntfn_slot = child_ntfn_slot;
    return 0;
}

/* ----------- v0.4.2 pulses ----------- */

int tm_pulse_send(pid_t sender_pid, seL4_CPtr connection_slot,
                  int priority, int code, int value)
{
    /* Resolve the sender's connection_slot → connection record →
     * target channel. The slot belongs to the *sender's* CSpace. */
    tm_connection_t *cn = connection_find_by_slot(sender_pid, connection_slot);
    if (!cn) return -EBADF;
    if (cn->channel_idx < 0 || cn->channel_idx >= TM_MAX_CHANNELS) return -EBADF;
    tm_channel_t *c = &g_channels[cn->channel_idx];
    if (!c->in_use) return -EBADF;

    if (c->pulse_count >= TM_PULSE_QUEUE_LEN) return -EAGAIN;

    int slot = c->pulse_tail;
    c->pulse_queue[slot].sender_pid = sender_pid;
    c->pulse_queue[slot].priority   = priority;
    c->pulse_queue[slot].code       = code;
    c->pulse_queue[slot].value      = value;

    c->pulse_tail = (slot + 1) % TM_PULSE_QUEUE_LEN;
    c->pulse_count++;
    return 0;
}

int tm_pulse_fetch(pid_t receiver_pid, seL4_CPtr recv_slot,
                   tm_pulse_t *out_pulse, int *out_scoid)
{
    /* Find the channel by (receiver_pid, recv_slot). */
    tm_channel_t *c = 0;
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (g_channels[i].in_use &&
            g_channels[i].owner_pid  == receiver_pid &&
            g_channels[i].owner_recv == recv_slot) {
            c = &g_channels[i];
            break;
        }
    }
    if (!c) return -EBADF;
    if (c->pulse_count == 0) return -ENOENT;

    int slot = c->pulse_head;
    *out_pulse = c->pulse_queue[slot];
    c->pulse_head = (slot + 1) % TM_PULSE_QUEUE_LEN;
    c->pulse_count--;

    /* scoid: server's view of the sender's connection. Find it. */
    *out_scoid = 0;
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        if (g_connections[i].in_use &&
            g_connections[i].channel_idx == (int)(c - g_channels) &&
            g_connections[i].client_pid  == out_pulse->sender_pid) {
            *out_scoid = (int)g_connections[i].badge;
            break;
        }
    }
    return 0;
}
