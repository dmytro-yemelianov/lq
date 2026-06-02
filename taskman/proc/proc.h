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
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <tm_limits.h>

/* CNode lookup depth for each process.  taskman's initThreadCNode has
 * a built-in guard such that the effective depth is 64.  Spawned
 * children's CNodes are freshly retyped (no guard), so radix = 12. */
#define TM_DEPTH_TASKMAN 64
#define TM_DEPTH_CHILD   12

/* Per-process credentials (v0.7).  Same six fields as QNX/QRV's
 * _cred_info — ruid/euid/suid + rgid/egid/sgid.  Inherited from
 * parent at posix_spawn; pid 1 (taskman) starts as root (all zero). */
typedef struct {
    uid_t ruid, euid, suid;
    gid_t rgid, egid, sgid;
} tm_cred_t;

/* One entry in a process's mmap tracker.  Records the (va_page,
 * frame_cap) pair the taskman-side mmap allocator produced so a
 * later TM_REQ_* handler can find the frame backing an arbitrary
 * caller-supplied VA -- needed for the spawn-args side-channel
 * (caller writes path/argv/envp into a mmap'd page; taskman reads
 * it via cap-copy + scratch-map at TM_REQ_SPAWN time).  `va_page`
 * is the start of the Mega_Page (2 MiB-aligned). */
typedef struct {
    unsigned long va_page;
    seL4_CPtr     frame;
} tm_mmap_entry_t;

typedef struct {
    int       in_use;
    pid_t     pid;
    seL4_CPtr cnode;
    seL4_CPtr next_slot;
    seL4_CPtr tcb;
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

    /* v0.7 cred — inherited from parent at spawn, settable via
     * setuid/setgid later. */
    tm_cred_t cred;

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
} tm_process_t;

typedef struct {
    int       in_use;
    pid_t     pid;
    int       tid;
    seL4_CPtr tcb_master;
    seL4_CPtr ntfn_master;
    seL4_CPtr tcb_in_caller;
    seL4_CPtr ntfn_in_caller;
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
 * allocated taskman-CSpace slot.  Returns the slot, or 0 on failure. */
seL4_CPtr taskman_alloc_and_retype(seL4_Word type, seL4_Word size_bits);

/* ----------- process: init + registry + lifecycle ----------- */

void          tm_init(seL4_CPtr ut, seL4_CPtr cnode_root, seL4_CPtr first_free);

int           tm_process_register(pid_t pid, seL4_CPtr cnode,
                                   seL4_CPtr tcb, seL4_CPtr vspace,
                                   seL4_CPtr first_free_slot);
tm_process_t *tm_process_lookup(pid_t pid);
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

/* v0.7 cred + ppid query — backs POSIX getpid/getppid/getuid/etc. */
int           tm_proc_self_info(pid_t caller_pid,
                                 pid_t *out_pid, pid_t *out_ppid,
                                 tm_cred_t *out_cred);

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
                            seL4_CPtr *out_recv_slot);
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
                      unsigned flags, seL4_CPtr *out_send_slot);
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

/* ----------- threads ----------- */

int tm_thread_alloc(pid_t caller_pid,
                    unsigned long stack_top_vaddr, unsigned stack_pages,
                    unsigned long ipc_vaddr,
                    unsigned prio, unsigned affinity,
                    int *out_tid,
                    seL4_CPtr *out_tcb_slot,
                    seL4_CPtr *out_ntfn_slot);

/* ----------- pulses ----------- */

int tm_pulse_send(pid_t sender_pid, seL4_CPtr connection_slot,
                  int priority, int code, int value);
int tm_pulse_fetch(pid_t receiver_pid, seL4_CPtr recv_slot,
                   tm_pulse_t *out_pulse, int *out_scoid);

#endif /* QSOE_TASKMAN_PROC_H */
