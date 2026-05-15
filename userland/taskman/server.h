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
#define TM_MAX_THREADS    256   /* across all processes */
#define TM_MAX_TID_PER_PROC 32  /* matches libqsoe's thread pool size */

/* v0.6.4: per-process mmap region.  Sits above stack/IPC-buffer
 * (which end at 0x200000) and runs upwards in 2 MiB-aligned chunks.
 * Same address libqsoe-side malloc.c uses for its bump-from-mmap
 * tracking. */
#define QSOE_MMAP_BASE  0x2000000UL    /* 32 MiB */
#define QSOE_MEGA_PAGE  0x200000UL     /* 2 MiB */

typedef struct {
    int       in_use;
    pid_t     pid;
    seL4_CPtr cnode;        /* caller's root CNode cap in taskman's CSpace */
    seL4_CPtr next_slot;    /* next free slot in caller's CSpace */
    seL4_CPtr tcb;          /* main TCB master in taskman's CSpace */
    seL4_CPtr vspace;
    seL4_CPtr untyped_budget;  /* v0.4.1: per-process untyped, in
                                  taskman's CSpace; copied into child
                                  slot QSOE_CAP_OWN_UNTYPED at spawn */
    /* v0.4.1: lazily-allocated PTs for the worker region at
     * [0x40000000, 0x40200000). Both 0 until first ThreadCreate.
     * The L1 covers [0x40000000, 0x80000000); the L0 covers the
     * first 2 MiB of that range, which holds up to 32 worker slots. */
    seL4_CPtr workers_l1_pt;
    seL4_CPtr workers_l0_pt;
    int       next_tid;     /* next free tid for this process (starts at 2) */
    /* v0.6.1 waitpid/procmgr_detach state.
     *
     * parent_pid    : pid that posix_spawn'd us. Set by spawn.c; gets
     *                 reparented to pid 1 on procmgr_detach.
     * exit_state    : 0 = alive, 1 = detached (alive, status delivered
     *                 to parent), 2 = exited (zombie waiting for
     *                 waitpid). Detached + exited later is still
     *                 exit_state=2 with the reparent reflected in
     *                 parent_pid.
     * exit_status   : status value the waiter should see — set by
     *                 procmgr_detach OR by exit/_exit.
     * waiter_reply_slot : if a parent is parked in waitpid() on us,
     *                 this is the taskman-CSpace slot holding the
     *                 saved reply cap. Zero = no parker. */
    pid_t     parent_pid;
    int       exit_state;
    int       exit_status;
    seL4_CPtr waiter_reply_slot;

    /* v0.6.4 signals-as-pulses.
     *
     * signal_chid : chid (in this process's own coid namespace) of
     *               the channel its signal thread listens on.  0 =
     *               not registered yet (process hasn't called
     *               TM_REQ_REGISTER_SIGNAL_CHID).  Set once at
     *               process startup by _qsoe_start_main, looked up
     *               by kill(pid, sig) → TM_REQ_GET_SIGNAL_CHID.
     */
    int       signal_chid;

    /* v0.6.4 Memory Manager state.
     *
     * mmap_top : next free 2 MiB-aligned vaddr in this process's
     *            address space, served by TM_REQ_MMAP.  Initialised by
     *            spawn.c to the bottom of the mmap region (above any
     *            image-side mappings).  Bumped upwards per request;
     *            v0.7+ will track per-mapping records for munmap. */
    unsigned long mmap_top;
} tm_process_t;

/* v0.4: per-thread registry entry. Master caps live in taskman's CSpace
 * so destroy/exit can revoke; the child gets copies via CNode_Copy. */
typedef struct {
    int       in_use;
    pid_t     pid;
    int       tid;
    seL4_CPtr tcb_master;       /* taskman-side TCB cap */
    seL4_CPtr ntfn_master;      /* taskman-side join Notification */
    seL4_CPtr tcb_in_caller;    /* slot the child got the TCB cap in */
    seL4_CPtr ntfn_in_caller;   /* slot the child got the Notification cap in */
} tm_thread_t;

/* v0.4.2 pulse queue: 8 entries per channel. Async fixed-size
 * messages; sender appends via tm_pulse_send, receiver pops via
 * tm_pulse_fetch on the same channel. Overflow → -EAGAIN (sender
 * may retry; v0.5 may resize). */
#define TM_PULSE_QUEUE_LEN 8

typedef struct {
    pid_t    sender_pid;
    int      priority;
    int      code;
    int      value;
} tm_pulse_t;

typedef struct {
    int       in_use;
    seL4_CPtr master;        /* taskman-side master cap slot */
    seL4_CPtr owner_recv;    /* owner's recv-cap slot (libqsoe's handle) */
    pid_t     owner_pid;
    int       owner_chid;
    unsigned  flags;
    /* v0.4.2 pulse ring buffer. */
    tm_pulse_t pulse_queue[TM_PULSE_QUEUE_LEN];
    int        pulse_head;   /* next slot to read */
    int        pulse_tail;   /* next slot to write */
    int        pulse_count;
    /* v0.4.3 bound-Notification wake. Allocated alongside the endpoint,
     * minted with badge=QSOE_NTFN_BADGE_BIT into ntfn_sig (taskman's
     * own CSpace), bound to the owner's TCB via TCB_BindNotification.
     * 0 if no Notification was set up (taskman's primary EP). */
    seL4_CPtr  ntfn_master;  /* unbadged master, used for Bind/Revoke */
    seL4_CPtr  ntfn_sig;     /* badged Send cap; taskman Signals via this */
} tm_channel_t;

typedef struct {
    int       in_use;
    int       channel_idx;   /* into the channel table */
    seL4_Word badge;
    pid_t     client_pid;
    seL4_CPtr client_slot;   /* the slot the client received */
    unsigned  flags;         /* v0.3.3: ConnectFlags state (COF_*) */
    /* v0.6.0: per-connection opaque context for stateful resmgrs.
     * cpiofs stashes (file_data_ptr, current_read_offset) here so
     * sequential reads on the same fd resume at the right place.
     * Other handlers can repurpose. Zeroed at registration time. */
    unsigned long ctx[2];
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

/* v0.4.1 pid allocator. Returns 0 if no pid available. tm_pid_free()
 * is called by ProcessTerminate to return the pid to the free list. */
pid_t          tm_pid_alloc(void);
void           tm_pid_free(pid_t pid);

/* v0.4.1 ProcessCreate plumbing. main() registers the embedded
 * userland CPIO and taskman's primary endpoint at boot; the wire
 * handler tm_process_create_by_name() looks up the ELF by name and
 * spawns it under a freshly-allocated pid. */
void           tm_set_userland_cpio(const void *start, unsigned long len);
void           tm_set_primary_ep(seL4_CPtr ep);
int            tm_process_create_by_name(const char *path, unsigned path_len,
                                         int argc, const char *const *argv,
                                         int envc, const char *const *envp,
                                         pid_t *out_pid);

/* v0.4.1 ProcessTerminate. Revokes the target process's master caps
 * (TCB, CNode, VSpace, worker TCBs/Notifications, owned channels) and
 * frees the pid. Returns 0 / -errno. status is reserved for v0.5
 * waitpid propagation. */
int            tm_process_terminate(pid_t target, int status);

/* ----------- v0.4.2 pulses ----------- */

/* Send a pulse to the channel referenced by the sender's connection
 * slot. sender_pid is the calling pid (from badge). */
int tm_pulse_send(pid_t sender_pid, seL4_CPtr connection_slot,
                  int priority, int code, int value);

/* Fetch the oldest pulse for the channel identified by the receiver's
 * recv_slot in its CSpace. Returns 0 / -ENOENT (empty queue). */
int tm_pulse_fetch(pid_t receiver_pid, seL4_CPtr recv_slot,
                   tm_pulse_t *out_pulse, int *out_scoid);

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

/* v0.6.1: return the taskman-side master cap for a channel index
 * (used by spawn.c when minting badged Send-caps into a freshly-
 * spawned child's CSpace, e.g. for inherited stdio). Returns 0 if
 * the index is out of range or the slot is unused. */
seL4_CPtr tm_channel_master(int idx);

/* v0.5.0: resolve which channel a badged message arrived through.
 * The dispatch loop uses this on IO_WRITE/IO_READ to route to the
 * right resmgr handler. Returns 0 + fills out_pid/out_chid; or
 * -ENOENT if the badge doesn't match a known connection. */
int tm_channel_by_badge(seL4_Word badge, pid_t *out_pid, int *out_chid);

/* v0.5.0: allocate a fresh scoid (also used as the cap's badge value).
 * Globally unique within the running taskman. spawn.c uses this when
 * minting stdio connections into a child that hasn't been registered
 * in the process table yet, so it can't go through tm_connect_attach. */
seL4_Word tm_alloc_scoid(void);

/* v0.6.1 procmgr_detach / waitpid plumbing. */

/* Set the parent pid of an already-registered process. Called by the
 * TM_REQ_PROCESS_CREATE handler right after a successful spawn so
 * the child knows whom to deliver its eventual exit/detach status
 * to. Returns 0 / -ESRCH. */
int tm_process_set_parent(pid_t child, pid_t parent);

/* Mark a process as detached: deliver `status` to its parent's
 * parked waitpid (if any), reparent to pid 1 so the child's later
 * real exit isn't reported to the original parent again, and let
 * the child continue running. Returns 0 on success. */
int tm_process_detach(pid_t pid, int status);

/* The blocking half of waitpid. If `child` has already detached or
 * exited, fills *out_status and returns 0 immediately. Otherwise
 * SaveCallers the parent's reply slot (allocated from taskman's
 * CSpace), parks it on the child's record, and returns 1 — the
 * dispatch loop sets out_no_reply and moves on. The Send happens
 * later, from tm_process_detach. Returns -errno on hard failures. */
int tm_process_waitpid(pid_t waiter, pid_t child,
                       int *out_status, int *out_parked);

/* Register a connection that was minted outside ConnectAttach (e.g.
 * spawn.c minting SYSMGR_COID into a child's slot 1). Returns 0 on
 * success. */
int tm_connection_register_existing(pid_t client_pid, seL4_CPtr client_slot,
                                    int channel_idx, seL4_Word badge,
                                    unsigned flags);

/* v0.6.0: get/set opaque per-connection context. Returns 0 on
 * success or -ENOENT if the badge doesn't name a live connection.
 * cpiofs uses ctx[0]=data_ptr, ctx[1]=read_offset. */
int tm_connection_set_ctx(seL4_Word badge, unsigned long c0, unsigned long c1);
int tm_connection_get_ctx(seL4_Word badge, unsigned long *c0, unsigned long *c1);

/* v0.6.0: look up the scoid badge for a connection identified by
 * (client_pid, slot). Used by TM_REQ_OPEN's cpiofs path to attach
 * per-fd state to the freshly-minted connection. Returns 0 on
 * success or -ENOENT. */
int tm_connection_badge_by_slot(pid_t client_pid, seL4_CPtr slot,
                                 seL4_Word *out_badge);

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

/* ----------- v0.4 thread allocator ----------- */

/* tm_thread_alloc — retype TCB + Notification + IPC frame + N stack frames,
 * map IPC and stack into the caller's VSpace, configure the TCB (same
 * CSpace+VSpace as the caller), copy TCB and Notification caps into the
 * caller's CSpace at fresh slots. The caller will WriteRegisters + Resume
 * itself.
 *
 *   stack_top_vaddr — top of stack range (sp starts here). Must be
 *                     page-aligned. Stack range is
 *                     [stack_top - stack_pages*4K, stack_top).
 *   ipc_vaddr       — where to map the IPC buffer page (page-aligned).
 *   prio            — initial priority (0..255).
 *
 * Returns 0 on success and fills *out_tid (assigned by taskman),
 * *out_tcb_slot, *out_ntfn_slot (slots in caller's CSpace). */
int tm_thread_alloc(pid_t caller_pid,
                    unsigned long stack_top_vaddr, unsigned stack_pages,
                    unsigned long ipc_vaddr,
                    unsigned prio, unsigned affinity,
                    int *out_tid,
                    seL4_CPtr *out_tcb_slot,
                    seL4_CPtr *out_ntfn_slot);

#endif /* QSOE_TASKMAN_SERVER_H */
