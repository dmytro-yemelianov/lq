/*
 * proc/proc.h — Process Manager: registries + APIs.
 *
 * Houses every datatype and handler in taskman's procmgr subsystem:
 *   - process registry (tm_process_t, cred, ppid, exit state)
 *   - thread registry (tm_thread_t)
 *   - channel registry (tm_channel_t, pulse queue)
 *   - connection registry (tm_connection_t)
 *   - shared CSpace allocator helpers
 *
 * libqsoe sees this header only when compiled with
 * -DQSOE_LIBQSOE_IN_TASKMAN.
 */
#ifndef QSOE_TASKMAN_PROC_H
#define QSOE_TASKMAN_PROC_H

#include "../sel4_types.h"
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <tm_limits.h>

/* CNode lookup depth for each process.  taskman's initThreadCNode has
 * a built-in guard such that the effective depth is 64.  Spawned
 * children's CNodes are freshly retyped (no guard), so radix = 12. */
#define TM_DEPTH_TASKMAN 64
#define TM_DEPTH_CHILD   12

/* Per-process untyped (pp_ut) reclamation (v0.10).
 *
 * Every process retypes its image frames, page tables and mmap
 * megapages from its OWN untyped rather than the shared master pool.
 * On seL4 an untyped's free watermark only rewinds when it becomes
 * child-free (src/object/untyped.c) -- the master pool never does
 * (taskman's own objects keep it populated), so per-spawn allocations
 * leaked it permanently.  A per-process untyped, by contrast, is
 * child-free the moment its process exits, so `Revoke` resets it and
 * it returns to a reuse free-list.  s_untyped then only ever hands out
 * pp_ut blocks, bounded by peak concurrent demand.
 *
 * TM_PP_UT_BITS sizes one block (2^24 = 16 MiB -- eight 2 MiB megapages,
 * so a typical whole process image+heap fits in one block without
 * overflowing); a process needing more grows by acquiring further
 * blocks, up to TM_PP_UT_PER_PROC.  Blocks are carved across every RAM
 * untyped (TM_RAM_UT_MAX of them); TM_PP_UT_FREE_MAX caps the reuse
 * free-list. */
#define TM_PP_UT_BITS       24
#define TM_PP_UT_PER_PROC   32
#define TM_PP_UT_FREE_MAX   64
#define TM_RAM_UT_MAX       32

/* Master-pool untyped size cap (2^28 = 256 MiB).  taskman's master pool
 * (s_untyped: primary endpoint, dispatcher reply, scheduling contexts,
 * taskman-self objects) draws from a single boot untyped.  seL4 hands
 * every boot untyped marked MAX_FREE_INDEX ("fully used"), so the FIRST
 * retype from it lazily clears the WHOLE block (seL4 never zeroes RAM at
 * boot).  Picking the largest untyped (the board's multi-GiB monster)
 * for the master pool therefore cleared gigabytes on the primary-endpoint
 * retype -- ~18 s on the FU740's 4 GiB block.  Cap the master untyped at
 * 256 MiB so that first clear is ~1 s; the multi-GiB untypeds stay in the
 * pp_ut pool (drawn ascending, so a typical workload never touches them)
 * and are cleared only if memory pressure actually reaches them. */
#define TM_MASTER_UT_MAX_BITS  28

/* Per-process recycle list for munmap'd Mega_Page frames (see the
 * mmap_free[] field below).  Sized to a process's plausible peak of
 * concurrently-freed-but-not-yet-remapped megapages; qsh's args-page
 * churn needs only 1, so 16 is generous headroom before the
 * delete-and-leak fallback kicks in. */
#define TM_MMAP_FREE_MAX    16

/* Per-process cap on cnode_copy'd device frames (MAP_PHYS shares).  The
 * PCI ECAM is 8 Mega_Pages; a driver also maps its BAR(s).  32 is ample. */
#define TM_MAX_DEVFRAMES    32

/* exit_state: a process's lifecycle stage, as seen by waitpid() and the
 * ps/sysinfo + /proc state columns.  0 is the live, attached default. */
enum {
    TM_EXIT_LIVE     = 0,   /* running, still attached to its parent       */
    TM_EXIT_DETACHED = 1,   /* procmgr_detach: alive daemon, parent reaped */
    TM_EXIT_ZOMBIE   = 2,   /* terminated, awaiting its waitpid() reap     */
};

/* Per-process object CNode (v0.10 slot reclamation).
 *
 * taskman holds a cap to every object it retypes for a process; in a
 * flat root CNode (4096 slots) the ~150 image-frame caps per spawn never
 * came back, capping the system at ~30 launches.  Each process instead
 * gets its own object CNode; the bulk write-once frame caps are MOVED
 * into it at the end of spawn (their root slots return to the free list
 * at once), and on exit the whole objcnode -- a pp_ut child -- is
 * destroyed by Revoke(pput), so all those slots vanish together.  The
 * default root CNode's guard fills all 64 address bits, so a cap in a
 * 2nd-level CNode cannot be INVOKED (page_map can't reach it); that is
 * why frames are mapped while still in a root slot and only then moved.
 *
 * Radix 2^9 = 512 slots, comfortably above SPAWN_MAX_FRAMES. */
#define TM_OBJCNODE_RADIX   9

/* Graceful crash (v0.10).  A child's fault handler is a badged
 * Send+GrantReply cap to taskman's primary endpoint; on a fatal U-mode
 * fault seL4 delivers a fault IPC there instead of wedging the thread
 * with "no fault handler".  The badge carries the faulter's pid OR'd
 * with TM_FAULT_BADGE_FLAG so the dispatcher tells a fault from a normal
 * request -- pids are < TM_MAX_PROCESSES (128), so bit 16 never collides
 * with a pid badge.  TM_SIG_SEGV is the terminating signal reported to
 * the parent's waitpid (musl WTERMSIG = status & 0x7f). */
#define TM_FAULT_BADGE_FLAG   0x10000UL
#define TM_FAULT_PID_MASK     (TM_FAULT_BADGE_FLAG - 1)
#define TM_SIG_SEGV           11

/* Per-process credentials use the canonical shared `struct _cred_info`
 * (<sys/qsoe.h>) -- the SAME type NQ + libtaskman use, so there is one
 * cred shape across both kernels.  (Was a private LQ `tm_cred_t` with an
 * identical first six fields; unified 2026-06-18.) */

/* Scheduling (v0.13).  QSOE adopts the QNX priority model verbatim:
 * priorities 0..255, higher number = higher priority, 0 reserved for the
 * per-CPU idle thread, user processes booting in the low unprivileged band
 * (QNX's default is around 10).  seL4 exposes the identical 0..255 range
 * (CONFIG_NUM_PRIORITIES = 256), so a QNX priority IS the seL4 priority --
 * SchedSet passes the value through untranslated.  Skimmer (NQ) adopts the
 * same 0..255 scale, so the one shared userspace means the same thing on
 * both kernels (a driver IST that raises itself to 21 elevates above the
 * default on either).
 *
 * Two roles one constant used to conflate are now separate:
 *   TM_PRIO_USER_DEFAULT -- the priority a freshly spawned user thread (and
 *      a default pthread worker) runs at.  A driver raises its interrupt
 *      service thread ABOVE it with SchedSet, exactly as its QNX code does
 *      (devb-nvme's IST -> 21).  Stays well below taskman so the server
 *      never starves.
 *   TM_SCHED_PRIO_MAX -- the ceiling SchedSet accepts (one below taskman).
 *      The QNX 1..63-unprivileged / >63-needs-PROCMGR_AID_PRIORITY split
 *      becomes a cred-gated policy once multi-user lands.
 *
 * taskman runs at seL4 InitThread priority (TM_PRIO_SYSTEM = 255) and blocks
 * on Recv when idle, so any user thread yields to it on demand.  Policy is
 * recorded and echoed by SchedGet but does not yet change SC budget/period
 * -- SCHED_RR and SCHED_FIFO both ride the budget==period round-robin SC;
 * the value must match <sched.h>'s SCHED_* (OTHER=0, FIFO=1, RR=2). */
#define TM_PRIO_USER_DEFAULT  10                    /* QNX default user prio */
#define TM_PRIO_SYSTEM        255                   /* taskman (seL4 InitThread) */
#define TM_SCHED_PRIO_MIN     1                     /* 0 reserved for idle */
#define TM_SCHED_PRIO_MAX     (TM_PRIO_SYSTEM - 1)  /* stay below taskman */
#define TM_SCHED_RR           2     /* default policy; matches <sched.h> SCHED_RR */

/* One entry in a process's mmap tracker.  Records the (va_page,
 * frame_cap) pair the taskman-side mmap allocator produced so a
 * later TM_REQ_* handler can find the frame backing an arbitrary
 * caller-supplied VA -- needed for the spawn-args side-channel
 * (caller writes path/argv/envp into a mmap'd page; taskman reads
 * it via cap-copy + scratch-map at TM_REQ_SPAWN time).  `va_page`
 * is the start of the Mega_Page (2 MiB-aligned). */

/* Per-thread name length for ps(1) -H rows (incl. NUL).  Matches the
 * 16-char cap of libc's qsoe_tcb_t.name.  Declared up here because both
 * tm_process_t (main-thread label) and tm_thread_t (worker labels) use
 * it. */
#define TM_THREAD_NAME_LEN  16

typedef struct {
    unsigned long va_page;
    seL4_CPtr     frame;
} tm_mmap_entry_t;

/* Per-process RELRO page registry (v0.13 real mprotect).  A page whose
 * VA lands in a loaded object's PT_GNU_RELRO range keeps an INVOKEABLE
 * frame cap here -- in its taskman root-CNode slot -- instead of moving
 * to the per-process objcnode like the bulk image frames (a cap there
 * can't be invoked, so Page_Map can't reach it; see the objcnode note
 * below).  TM_REQ_MPROTECT re-Page_Maps these with the requested rights,
 * which is exactly what rtld's RELRO pass needs (make .got / .data.rel.ro
 * read-only after relocation).  Main image + libc.so + rtld each
 * contribute only a few pages, so 32 is ample and the root-slot cost is
 * negligible next to the ~150 bulk frames the objcnode absorbs. */
typedef struct {
    unsigned long va_page;   /* page-aligned child VA */
    seL4_CPtr     frame;     /* invokeable frame cap (taskman root slot) */
} tm_mprot_entry_t;
#define TM_MAX_MPROT  32

typedef struct {
    int       in_use;
    pid_t     pid;
    seL4_CPtr cnode;
    seL4_CPtr next_slot;
    seL4_CPtr tcb;
    /* v0.10 main-thread MCS scheduling context.  Retyped into taskman's
     * root CNode at spawn (tm_sched_context_create); its object is a
     * pp_ut child destroyed by Revoke(pput) on exit, but the root-CNode
     * SLOT must be returned to the allocator explicitly in teardown --
     * otherwise every spawn leaks one slot and the root CNode fills. */
    seL4_CPtr sc;
    /* v0.13 main-thread scheduling state, tracked here because seL4 has no
     * get-priority invocation -- SchedGet reads these back.  Seeded at spawn
     * (TM_PRIO_USER_DEFAULT / TM_SCHED_RR) and updated by SchedSet. */
    int       sched_prio;
    int       sched_policy;
    seL4_CPtr vspace;
    seL4_CPtr untyped_budget;
    seL4_CPtr workers_l1_pt;
    seL4_CPtr workers_l0_pt;
    int       next_tid;

    /* v0.6.1 waitpid / procmgr_detach state. */
    pid_t     parent_pid;
    int       exit_state;
    int       exit_status;
    seL4_CPtr waiter_reply_slot;

    /* v0.6.4 signals-as-pulses: chid of this process's signal channel. */
    int       signal_chid;

    /* v0.6.4 mmap region top — bumped upwards per TM_REQ_MMAP. */
    unsigned long mmap_top;

    /* Per-process (va_page, frame_cap) tracker.  Each TM_REQ_MMAP
     * Mega_Page allocation appends one entry; the spawn-args
     * side-channel (TM_REQ_SPAWN reading from a caller-mmap'd page)
     * looks the frame up by (va & ~(2 MiB - 1)).  Bounded by
     * TM_MAX_MMAP_PER_PROC; overflow crashes loudly per the
     * stubs-announce / silent-truncate ban. */
    tm_mmap_entry_t mmap[TM_MAX_MMAP_PER_PROC];
    int             mmap_count;

    /* v0.10 munmap'd-megapage recycle list.  The pp_ut allocator is
     * bump-only: deleting a Mega_Page frame cap does NOT return its
     * 2 MiB to the parent untyped's free index (only Revoke at process
     * exit does).  So a long-lived process that churns mmap/munmap --
     * notably qsh, which mmaps + munmaps a fresh 2 MiB args page on
     * every posix_spawn -- would otherwise leak 2 MiB per cycle and
     * march the whole RAM pool to exhaustion.  Instead munmap parks the
     * still-valid frame cap here (unmapped) and the next mmap re-maps it
     * at the new VA, so the 2 MiB is genuinely reused within the
     * process.  Bounded; on overflow munmap falls back to delete (that
     * one frame's space leaks until exit, but overflow is rare since a
     * process's live+free megapages are bounded by its working set). */
    seL4_CPtr mmap_free[TM_MMAP_FREE_MAX];
    int       mmap_free_count;

    /* v0.11 device-frame copies for MAP_PHYS.  A shared device region
     * (e.g. the PCI ECAM) is carved ONCE into taskman's device-map
     * registry; each requester gets cnode_copy's of those frame caps
     * mapped into its own VSpace.  These copy caps live in taskman's
     * root CNode and must be deleted on exit (the underlying frame
     * survives -- it is the registry's, shared with other mappers).
     * Kept apart from mmap[] so the anon-megapage munmap/recycle path
     * never mistakes a shared device frame for reclaimable RAM. */
    seL4_CPtr     devframes[TM_MAX_DEVFRAMES];
    unsigned long devframe_va[TM_MAX_DEVFRAMES];  /* VA each cap maps; lets
                                                   * munmap find device frames
                                                   * (they aren't in mmap[]) */
    int           devframe_count;

    /* v0.13 RELRO page registry (see tm_mprot_entry_t).  Populated at
     * spawn for pages in any loaded object's PT_GNU_RELRO range; consulted
     * by tm_mprotect_serve.  Slots reclaimed in teardown (the frame itself
     * dies with Revoke(pput), same as the mmap megapages). */
    tm_mprot_entry_t mprot[TM_MAX_MPROT];
    int              mprot_count;

    /* v0.7 cred — inherited from parent at spawn, settable via
     * setuid/setgid later. */
    struct _cred_info cred;

    /* v0.7 current working directory.  Stored as an absolute path,
     * NUL-terminated; "/" for the freshly-spawned init and inherited
     * by children at spawn.  Updated by chdir(); read by getcwd().
     * 256 bytes is QSOE's de-facto path length cap. */
    char      cwd[256];

    /* v0.7 file-creation mask.  Default 022 per POSIX; inherited by
     * children at spawn; mutated by umask(). */
    unsigned  umask;

    /* v0.7 ITIMER_REAL state.  When `itimer_expiry_ticks` is
     * non-zero, the timer is armed; the timer sweep at each
     * dispatch entry checks it.  `itimer_interval_ticks` is the
     * re-arm value (0 = one-shot, disarmed after firing).  Sigevent
     * is delivered as a SIGALRM pulse to this process's signal
     * channel (see signal_chid above). */
    unsigned long itimer_expiry_ticks;
    unsigned long itimer_interval_ticks;

    /* v0.10 process name (basename of the spawned ELF), for /proc.
     * Captured at spawn from elf_name; NUL-terminated, truncated. */
    char      name[32];

    /* ps(1) -H label for the main thread (tid 1).  The main thread lives
     * here in the process record, not in g_threads, so it can't be tagged
     * via tm_thread_find; ThreadCtl(TCTL_NAME) from the main thread lands
     * here instead.  "" until set -> ps falls back to the process name. */
    char      main_name[TM_THREAD_NAME_LEN];

    /* v0.10 per-process untyped blocks (see TM_PP_UT_* above).  Every
     * image frame / page table / mmap megapage is retyped from one of
     * these; on exit each is Revoked + returned to the reuse free-list,
     * which is how this process's RAM is actually reclaimed. */
    seL4_CPtr pput[TM_PP_UT_PER_PROC];
    int       pput_count;

    /* v0.10 per-process object CNode (see TM_OBJCNODE_RADIX above): holds
     * the image-frame caps moved out of the root CNode at spawn.  A
     * pp_ut child, so Revoke(pput) destroys it on exit; only its single
     * root-CNode slot is freed back to the slot free-list. */
    seL4_CPtr objcnode;
    int       objcnode_next;

    /* v0.10 fault handler: the badged Send+GrantReply cap to taskman's
     * primary EP installed as the main thread's fault endpoint.  Stays
     * in taskman's CSpace (the TCB references it); its slot is freed in
     * teardown once the TCB is gone. */
    seL4_CPtr fault_ep;
} tm_process_t;

typedef struct {
    int       in_use;
    pid_t     pid;
    int       tid;
    seL4_CPtr tcb_master;
    seL4_CPtr ntfn_master;
    seL4_CPtr tcb_in_caller;
    seL4_CPtr ntfn_in_caller;
    /* v0.10 MCS scheduling context for this worker, in taskman's root
     * CNode.  Like the main thread's, its slot must be freed in teardown
     * (the object dies with Revoke(pput); the slot does not). */
    seL4_CPtr sc;
    /* v0.13 scheduling state (see tm_process_t).  Seeded at TM_REQ_THREAD_ALLOC
     * from the requested prio (TM_SCHED_RR policy); updated by SchedSet. */
    int       sched_prio;
    int       sched_policy;
    /* Short thread name for ps(1) -H rows; "" until set.  taskman can't
     * see libc's ThreadCtl(TCTL_NAME) (that writes the libc-local TCB),
     * so this is populated only where taskman already knows the role --
     * e.g. tm_channel_bind_thread tags the system thread "sigthread". */
    char      name[TM_THREAD_NAME_LEN];
} tm_thread_t;

#define TM_PULSE_QUEUE_LEN 8

typedef struct {
    pid_t    sender_pid;
    int      priority;
    int      code;
    int      value;
} tm_pulse_t;

typedef struct {
    int        in_use;
    seL4_CPtr  master;
    seL4_CPtr  owner_recv;
    pid_t      owner_pid;
    int        owner_chid;
    unsigned   flags;
    tm_pulse_t pulse_queue[TM_PULSE_QUEUE_LEN];
    int        pulse_head;
    int        pulse_tail;
    int        pulse_count;
    seL4_CPtr  ntfn_master;
    seL4_CPtr  ntfn_sig;
} tm_channel_t;

typedef struct {
    int           in_use;
    int           channel_idx;
    seL4_Word     badge;
    pid_t         client_pid;
    seL4_CPtr     client_slot;
    seL4_CPtr     ntfn_slot;    /* direct-pulse Send-cap in the client's
                                 * CSpace (QSOE_CHF_PULSE_DIRECT), else 0 */
    unsigned      flags;
    unsigned long ctx[2];
} tm_connection_t;

/* ----------- shared CSpace state (defined in process.c) ----------- */

extern seL4_CPtr s_untyped;
extern seL4_CPtr s_cnode_root;
extern seL4_CPtr s_next_slot;

/* Per-pid CNode lookup depth. */
seL4_Uint8 cnode_depth_for(pid_t pid);

/* CSpace slot allocator: pop from the free list, else bump s_next_slot. */
seL4_CPtr taskman_alloc_empty_slot(void);
void      taskman_free_slot(seL4_CPtr slot);

/* Retype an untyped of the given seL4 object type into a freshly-
 * allocated taskman-CSpace slot.  Returns the slot, or 0 on failure.
 * Draws from the active per-process untyped context (see tm_pput_*)
 * when one is set -- otherwise from the shared master pool. */
seL4_CPtr taskman_alloc_and_retype(seL4_Word type, seL4_Word size_bits);

/* ----------- per-process untyped (pp_ut) context (v0.10) -----------
 *
 * Bracket the per-process retypes of a spawn or an mmap so they draw
 * from a per-process untyped that can be reclaimed wholesale on exit.
 *
 *   tm_pput_spawn_begin  acquire the first pp_ut into staging[0] and
 *                        make subsequent taskman_alloc_and_retype calls
 *                        draw from it (growing into staging[] on
 *                        overflow).  Returns 0 on success, -ENOMEM if
 *                        the master pool is exhausted.  The caller
 *                        transfers staging[] into the new record on
 *                        success, or releases it on failure.
 *   tm_pput_proc_begin   same, but for a process that already owns
 *                        pp_ut blocks (mmap): grows p->pput[] directly.
 *   tm_pput_end          clear the context (back to the master pool).
 *   tm_pput_release_list Revoke each block + return it to the reuse
 *                        free-list (teardown). */
int  tm_pput_spawn_begin(seL4_CPtr *staging, int *staging_n);
void tm_pput_proc_begin(tm_process_t *p);
void tm_pput_end(void);
void tm_pput_release_list(seL4_CPtr *list, int n);

/* Register the RAM untypeds pp_ut blocks are carved from (called once
 * at boot with every RAM untyped >= one block wide). */
void tm_pput_pool_init(const seL4_CPtr *uts, int n);

/* In-taskman status-returning IPC send (qsoe/msg.c): like MsgSend but
 * returns the server's reply status; the payload is left in the IPC
 * buffer (count at msg[0], data from msg[4]). */
int tm_msg_call(int coid, const void *smsg, int sbytes);

/* Spawn-from-filesystem (proc/spawn.c).  A binary not in the boot cpio is
 * read off a mounted resmgr (fs-qrv) into a scratch window in taskman's
 * own VSpace, then handed to tm_spawn.  ctx records the pp_ut block + the
 * mapped megaframes so tm_spawn_fs_unload reclaims them once tm_spawn has
 * copied the image into the child.  out_blob points into taskman's VSpace
 * and is valid only until tm_spawn_fs_unload. */
#define TM_FS_MAX_MF  8           /* read-buffer cap: 8 * 2 MiB = 16 MiB */
typedef struct {
    seL4_CPtr mf[TM_FS_MAX_MF];   /* mapped megaframe caps                */
    int       nmf;
    seL4_CPtr pput[TM_PP_UT_PER_PROC];   /* read-buffer pp_ut block(s)    */
    int       pput_n;
} tm_fs_load_t;
int  tm_spawn_fs_load(const char *path, const void **out_blob,
                      unsigned long *out_size, tm_fs_load_t *ctx);
void tm_spawn_fs_unload(tm_fs_load_t *ctx);

/* ----------- MCS scheduling + reply objects (v0.10, process.c) ----------- */

/* SchedControl base + node count (from bootinfo.schedcontrol), set by
 * main().  tm_sched_control_for_core() picks the cap for a core — that
 * placement is how MCS expresses thread affinity. */
void      tm_set_sched_control(seL4_CPtr base, seL4_Word num_nodes);
seL4_CPtr tm_sched_control_for_core(unsigned core);

/* Create + configure a round-robin scheduling context on `core`.
 * Every spawned/created TCB needs one bound (SetSchedParams) to run. */
seL4_CPtr tm_sched_context_create(unsigned core);

/* Retype a bare reply object into a fresh taskman slot. */
seL4_CPtr tm_reply_object_create(void);

/* Dispatcher reply object: tm_reply_init() allocates it once (before the
 * first Recv); tm_active_reply() returns it for the Recv/ReplyRecv calls;
 * tm_reply_park() stashes the current caller's reply + replenishes, and
 * tm_reply_deliver() answers a parked reply later (MCS replacement for
 * the SaveCaller + Send deferred-reply pattern). */
int       tm_reply_init(void);
seL4_CPtr tm_active_reply(void);
seL4_CPtr tm_reply_park(void);
void      tm_reply_deliver(seL4_CPtr slot, seL4_MessageInfo_t tag,
                           seL4_Word mr0, seL4_Word mr1,
                           seL4_Word mr2, seL4_Word mr3);
void      tm_reply_drop(seL4_CPtr slot);

/* ----------- process: init + registry + lifecycle ----------- */

void          tm_init(seL4_CPtr ut, seL4_CPtr cnode_root, seL4_CPtr first_free);

int           tm_process_register(pid_t pid, seL4_CPtr cnode,
                                   seL4_CPtr tcb, seL4_CPtr vspace,
                                   seL4_CPtr first_free_slot);
tm_process_t *tm_process_lookup(pid_t pid);
tm_process_t *tm_process_by_index(int idx);   /* /proc enumeration */
seL4_CPtr     tm_process_alloc_slot(pid_t pid);

pid_t         tm_pid_alloc(void);
void          tm_pid_free(pid_t pid);

void          tm_set_userland_cpio(const void *start, unsigned long len);
const void   *tm_get_userland_cpio_start(void);
unsigned long tm_get_userland_cpio_len(void);
void          tm_set_primary_ep(seL4_CPtr ep);
int           tm_process_create_by_name(const char *path, unsigned path_len,
                                         int argc, const char *const *argv,
                                         int envc, const char *const *envp,
                                         pid_t *out_pid);
int           tm_process_terminate(pid_t target, int status);

/* Handle a fault IPC delivered to the primary EP (badge carried
 * TM_FAULT_BADGE_FLAG): log it and terminate the faulting process so a
 * U-mode crash never wedges the kernel.  fault_type is the seL4 fault
 * label from the IPC's MessageInfo. */
void          tm_handle_fault(pid_t pid, unsigned fault_type);

int           tm_process_set_parent(pid_t child, pid_t parent);
int           tm_process_detach(pid_t pid, int status);
int           tm_process_waitpid(pid_t waiter, pid_t child,
                                  int *out_status, int *out_parked);

/* Look up the (taskman-side) frame cap backing the Mega_Page that
 * contains `va` in `proc`'s VSpace.  Returns the cap or 0 if no
 * tracked mmap region covers `va`.  Backs the spawn-args
 * side-channel: the caller mmap'd a page and wrote args into it;
 * taskman wants to cap-copy + scratch-map the frame to read those
 * args back.  Match is by (va & ~(QSOE_MEGA_PAGE - 1)). */
seL4_CPtr     tm_process_find_frame(const tm_process_t *proc,
                                     unsigned long va);

/* Read up to 4 KiB from `args_va` in `proc`'s VSpace into out_buf.
 * Looks up the Mega_Page frame backing args_va via tm_process_find_frame,
 * cap-copies it, scratch-maps the copy in taskman's vspace, memcpys,
 * unmaps, and deletes the copy.  Backs TM_REQ_SPAWN's side-channel
 * read-back of the packed (path/argv/envp) blob. */
int           tm_spawn_read_args(tm_process_t *proc, unsigned long args_va,
                                  unsigned len, void *out_buf);

/* v0.10 zero a free (unmapped) Mega_Page frame via taskman's scratch
 * mapping, so tm_munmap_serve can recycle frames while preserving
 * MAP_ANONYMOUS zero-fill.  Returns 0 on success, negative errno. */
int           tm_zero_megaframe(seL4_CPtr frame);

/* v0.13 bulk-IPC bounce copy (doc/plans/bulk_ipc.txt).  Copies `len`
 * bytes between two processes' buffers, one Mega_Page chunk at a time,
 * by cap-copying + scratch-mapping each side's frame (taskman holds the
 * caps in proc->mmap[]).  Buffers may have any in-megaframe offset and
 * must be heap/mmap-backed (not image/stack).  Returns bytes copied or
 * negative errno (-EFAULT if a VA isn't backed by a tracked frame). */
long          tm_bulk_copy(tm_process_t *src_proc, unsigned long src_va,
                            tm_process_t *dst_proc, unsigned long dst_va,
                            unsigned long len);

/* TM_REQ_MSG_XFER backends.  PULL copies the blocked client's send buffer
 * into the calling server's receive buffer and stashes the client's reply
 * buffer; PUSH copies the server's reply buffer into that stashed client
 * reply buffer.  `server_pid` is the dispatch caller; `client_pid` is the
 * blocked sender (the server's MsgReceive badge).  Return bytes or -errno. */
long          tm_msg_xfer_pull(pid_t server_pid, pid_t client_pid,
                               unsigned long client_src_va,
                               unsigned long server_dst_va, unsigned long len,
                               unsigned long client_rbuf_va,
                               unsigned long client_rbytes);
long          tm_msg_xfer_push(pid_t server_pid, pid_t client_pid,
                               unsigned long server_src_va, unsigned long len);

/* v0.7 cred + ppid query — backs POSIX getpid/getppid/getuid/etc. */
int           tm_proc_self_info(pid_t caller_pid,
                                 pid_t *out_pid, pid_t *out_ppid,
                                 struct _cred_info *out_cred);

/* v0.7 cwd accessors.  path bytes for chdir come in via msg[4..]
 * (length in path_len); getcwd writes the cwd into msg[4..] and
 * returns its length in *out_len. */
int           tm_chdir(pid_t caller_pid, unsigned path_len);
int           tm_getcwd(pid_t caller_pid, unsigned *out_len);

/* v0.7 dup-cap: copy `src_slot` into `dest_slot` inside the caller's
 * own CSpace.  Backs POSIX dup2 / fcntl F_DUPFD. */
int           tm_dup_cap(pid_t caller_pid,
                          seL4_CPtr src_slot, seL4_CPtr dest_slot);

/* v0.7 umask: if set != -1, install the new mask; return the old
 * mask in *out_old either way. */
int           tm_umask(pid_t caller_pid, int set, unsigned *out_old);

/* v0.7 cred mutation.  Each *_new value of 0xFFFFFFFF means "leave
 * this field alone".  No privilege check yet; v0.8+ verifies euid==0
 * before applying.  Backs setuid / setgid / setresuid / setresgid. */
int           tm_set_cred(pid_t caller_pid,
                           unsigned ruid_new, unsigned euid_new,
                           unsigned suid_new,
                           unsigned rgid_new, unsigned egid_new,
                           unsigned sgid_new);

/* v0.13 scheduling.  SchedSet sets thread (pid,tid)'s seL4 priority via
 * TCB_SetPriority and records its policy; SchedGet reads both back.  pid==0
 * means the caller; tid<=1 selects the process's main thread (p->tcb), a
 * higher tid a TM_REQ_THREAD_ALLOC worker (tm_thread_find).  Return 0 / a
 * negative errno (-ESRCH unknown thread, -EINVAL bad priority/policy). */
int           tm_sched_set(pid_t caller_pid, pid_t pid, int tid,
                           int policy, int prio);
int           tm_sched_get(pid_t caller_pid, pid_t pid, int tid,
                           int *out_policy, int *out_prio);

/* v0.7 timer subsystem (hybrid lazy expiry).  See proc/timer.c.
 *
 * tm_timer_sweep()  — called at every dispatch entry; wakes any
 *                     sleepers whose deadlines have passed and
 *                     pulses SIGALRM for any expired itimer.
 * tm_nanosleep()    — parks the caller's reply cap; returns
 *                     "no_reply" sentinel via *out_parked = 1.
 * tm_setitimer()    — arms/disarms the per-process ITIMER_REAL.
 */
void          tm_timer_sweep(void);
int           tm_nanosleep(pid_t caller_pid, unsigned long total_ns,
                           int *out_parked);
int           tm_setitimer(pid_t caller_pid, int which,
                           unsigned long value_us,
                           unsigned long interval_us,
                           unsigned long *out_old_value_us,
                           unsigned long *out_old_interval_us);

/* ----------- channels ----------- */

int       tm_channel_create(pid_t owner_pid, int chid, unsigned flags,
                            int creator_tid, seL4_CPtr *out_recv_slot,
                            int *out_chid);
int       tm_channel_destroy(pid_t owner_pid, seL4_CPtr recv_slot);
int       tm_channel_register_existing(pid_t pid, int chid,
                                        seL4_CPtr master_slot,
                                        seL4_CPtr recv_slot);
int       tm_channel_index(pid_t pid, int chid);
seL4_CPtr tm_channel_master(int idx);
seL4_Word tm_alloc_scoid(void);

/* Live channel-table access for connect.c / pulse.c. */
tm_channel_t *tm_channels_array(void);   /* returns &g_channels[0] */
tm_thread_t  *tm_threads_array(void);    /* returns &g_threads[0] */

/* ----------- connections ----------- */

int tm_connect_attach(pid_t client_pid, pid_t target_pid, int target_chid,
                      unsigned flags, seL4_CPtr *out_send_slot,
                      seL4_CPtr *out_ntfn_slot);
int tm_connect_detach(pid_t client_pid, seL4_CPtr send_slot);
int tm_connect_server_info(pid_t caller_pid, seL4_CPtr client_slot,
                            pid_t *out_server_pid, int *out_server_chid,
                            seL4_Word *out_scoid);
int tm_connect_client_info(seL4_Word scoid,
                            pid_t *out_client_pid, pid_t *out_sid,
                            unsigned *out_flags);
int tm_connect_flags(pid_t caller_pid, seL4_CPtr client_slot,
                     unsigned mask, unsigned bits, unsigned *out_old);

int tm_connection_register_existing(pid_t client_pid, seL4_CPtr client_slot,
                                     int channel_idx, seL4_Word badge,
                                     unsigned flags);
/* Clone the source connection's registry row at (client_pid, src_slot)
 * into a fresh row keyed by dest_slot.  Used by tm_dup_cap so each
 * dup'd fd has an independent registry entry — closing one fd no
 * longer invalidates the other.  Ctx is copied (independent offsets;
 * POSIX strict-offset-sharing is a follow-up). */
int tm_connection_clone_for_dup(pid_t client_pid, seL4_CPtr src_slot,
                                 seL4_CPtr dest_slot);
int tm_connection_set_ctx(seL4_Word badge, unsigned long c0, unsigned long c1);
int tm_connection_get_ctx(seL4_Word badge, unsigned long *c0, unsigned long *c1);
int tm_connection_badge_by_slot(pid_t client_pid, seL4_CPtr slot,
                                 seL4_Word *out_badge);

/* Used by main.c's dispatcher to route IO_* by badge. */
int tm_channel_by_badge(seL4_Word badge, pid_t *out_pid, int *out_chid);

/* Resolve a connection badge (scoid) to the owning client pid; 0 if none.
 * Used by bulk IPC to map a server's MsgReceive badge to the sender. */
pid_t tm_connection_client_pid(seL4_Word badge);

/* ----------- threads ----------- */

int tm_thread_alloc(pid_t caller_pid,
                    unsigned long stack_top_vaddr, unsigned stack_pages,
                    unsigned long ipc_vaddr,
                    unsigned prio, unsigned affinity,
                    int *out_tid,
                    seL4_CPtr *out_tcb_slot,
                    seL4_CPtr *out_ntfn_slot,
                    seL4_CPtr *out_reply_slot);

/* Look up a ThreadCreate'd thread by (pid, tid).  Returns NULL if no
 * such live entry; the main thread (tid 1) is NOT in g_threads (it is
 * tm_process_t.tcb) and so is never returned here. */
tm_thread_t *tm_thread_find(pid_t pid, int tid);

/* TM_REQ_CHANNEL_BIND_THREAD: rebind chid's pulse Notification from the
 * process's main TCB to its system thread `tid`, and tag that thread
 * "sigthread" for ps(1).  See <qsoe/slots.h>. */
int tm_channel_bind_thread(pid_t owner_pid, int chid, int tid);

/* ----------- pulses ----------- */

int tm_pulse_send(pid_t sender_pid, seL4_CPtr connection_slot,
                  int priority, int code, int value);
int tm_pulse_fetch(pid_t receiver_pid, seL4_CPtr recv_slot,
                   tm_pulse_t *out_pulse, int *out_scoid);

#endif /* QSOE_TASKMAN_PROC_H */
