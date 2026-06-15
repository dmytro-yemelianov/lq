/*
 * proc/process.c — process registry, pid alloc, CSpace allocator,
 * lifecycle (create / detach / waitpid / terminate), and the v0.7
 * tm_proc_self_info handler that backs POSIX getpid/getppid/getuid/etc.
 *
 * Split out of v0.6.4's server.c.  Owns the g_processes[] table and
 * the CSpace bump allocator (s_next_slot + s_slot_free_list); the
 * sibling files in proc/ (channel.c, connect.c, thread.c, pulse.c)
 * use the helpers exposed in proc.h.
 */

#include "proc.h"
#include "spawn.h"
#include "../mem/mem.h"   /* QSOE_MMAP_BASE */
#include "../qsoe_invoke.h"
#include "../path/cpiofs.h"  /* tm_cpio_lookup */
#include <tm_log.h>
#include <qsoe/slots.h>
#include <cpio.h>

static tm_process_t g_processes[TM_MAX_PROCESSES];

/* Shared globals — referenced by channel.c, connect.c, thread.c
 * (extern in proc.h).  Kept non-static so spawn.c (one dir up) can
 * still bump s_next_slot during boot. */
seL4_CPtr s_untyped;
seL4_CPtr s_cnode_root;
seL4_CPtr s_next_slot;

/* ----------- MCS scheduling + reply-object state (v0.10) ----------- */

/* Base of the per-core SchedControl cap region (bootinfo.schedcontrol.
 * start) and the node count, set by main() via tm_set_sched_control().
 * Configuring a scheduling context on core N invokes s_sched_control +
 * N; that placement replaces the retired non-MCS TCB_SetAffinity. */
static seL4_CPtr s_sched_control;
static seL4_Word s_num_nodes = 1;

/* The dispatcher's "active" reply object — bound to the current caller
 * by every Recv/ReplyRecv (see main.c's loop).  Deferred-reply handlers
 * move this cap aside (tm_reply_park) and a fresh reply object replaces
 * it for the next receive. */
static seL4_CPtr s_active_reply;

void tm_set_sched_control(seL4_CPtr base, seL4_Word num_nodes)
{
    s_sched_control = base;
    s_num_nodes     = num_nodes ? num_nodes : 1;
}

seL4_CPtr tm_sched_control_for_core(unsigned core)
{
    if (core >= s_num_nodes) core = 0;
    return s_sched_control + core;
}

/* v0.4.1 pid allocator.  pid 1 is reserved for taskman; child pids
 * start at 2 and are bump-allocated.  Freed pids land on a LIFO. */
static pid_t s_next_pid       = 2;
static pid_t s_pid_free_list[TM_MAX_PROCESSES];
static int   s_pid_free_count = 0;

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
    if (pid <= QSOE_PID_TASKMAN) return;
    if (s_pid_free_count < TM_MAX_PROCESSES) {
        s_pid_free_list[s_pid_free_count++] = pid;
    }
}

/* Handles for the embedded userland CPIO and taskman's primary
 * endpoint, set by main() after boot.  ProcessCreate uses them to
 * locate ELFs and badge SYSMGR caps into new children. */
static const void   *s_cpio_start;
static unsigned long s_cpio_len;
static seL4_CPtr     s_primary_ep;

void tm_set_userland_cpio(const void *start, unsigned long len)
{
    s_cpio_start = start;
    s_cpio_len   = len;
}

const void *tm_get_userland_cpio_start(void) { return s_cpio_start; }
unsigned long tm_get_userland_cpio_len(void)   { return s_cpio_len; }

void tm_set_primary_ep(seL4_CPtr ep)
{
    s_primary_ep = ep;
}

int tm_process_create_by_name(const char *path, unsigned path_len,
                              int argc, const char *const *argv,
                              int envc, const char *const *envp,
                              pid_t *out_pid)
{
    if (path_len == 0 || path_len >= 60) return -EINVAL;
    if (!s_cpio_start || !s_primary_ep) return -EINVAL;

    /* Two lookup strategies:
     *   1. Absolute path ("/sbin/devc-ser8250"): strip leading '/' and
     *      look up directly via cpiofs (handles symlinks too).
     *   2. Bare name ("qsh"): try "bin/<name>" first, then
     *      "sbin/<name>" — a minimal PATH search until the libc gains
     *      proper PATH-based execvp.  Lets init.sh write
     *      `devc-ser8250 &` instead of always typing the full path. */
    char name[64];
    unsigned long elf_size = 0;
    const void *elf = 0;

    if (path[0] == '/') {
        if (path_len < 2 || path_len > sizeof name) return -EINVAL;
        for (unsigned i = 1; i < path_len; ++i) name[i - 1] = path[i];
        name[path_len - 1] = 0;
        elf = tm_cpio_lookup(name, &elf_size);
    } else {
        /* Try bin/ then sbin/. */
        static const char *prefixes[] = { "bin/", "sbin/" };
        static const unsigned prefix_lens[] = { 4, 5 };
        for (int pi = 0; pi < 2 && !elf; ++pi) {
            if (prefix_lens[pi] + path_len + 1 > sizeof name) return -EINVAL;
            for (unsigned i = 0; i < prefix_lens[pi]; ++i) {
                name[i] = prefixes[pi][i];
            }
            for (unsigned i = 0; i < path_len; ++i) {
                name[prefix_lens[pi] + i] = path[i];
            }
            name[prefix_lens[pi] + path_len] = 0;
            elf = tm_cpio_lookup(name, &elf_size);
        }
    }
    if (!elf) return -ENOENT;

    pid_t new_pid = tm_pid_alloc();
    if (!new_pid) return -ENOMEM;

    /* elf_name passed to tm_spawn is the basename — that's what
     * spawn_name_eq cares about, and shebang code uses it for the
     * synthesised script_path argument. */
    const char *basename = name;
    for (const char *p = name; *p; ++p) if (*p == '/') basename = p + 1;

    /* Bracket the spawn's object retypes with a per-process untyped so
     * the whole image is reclaimable on exit (see tm_pput_*).  Staged
     * locally because the process record does not exist until tm_spawn
     * registers it mid-flight. */
    seL4_CPtr staging[TM_PP_UT_PER_PROC];
    int       staging_n = 0;
    if (tm_pput_spawn_begin(staging, &staging_n) != 0) {
        tm_pid_free(new_pid);
        return -ENOMEM;
    }

    int sr = tm_spawn(elf, elf_size, new_pid, s_primary_ep,
                       argc, argv, envc, envp, basename);
    tm_pput_end();
    if (sr) {
        tm_pput_release_list(staging, staging_n);
        tm_pid_free(new_pid);
        return sr;
    }
    /* Hand the staged block(s) to the now-registered record so teardown
     * reclaims them. */
    tm_process_t *prec = tm_process_lookup(new_pid);
    if (prec) {
        for (int i = 0; i < staging_n; ++i) prec->pput[i] = staging[i];
        prec->pput_count = staging_n;
    }
    *out_pid = new_pid;
    return 0;
}

void tm_init(seL4_CPtr ut, seL4_CPtr cnode_root, seL4_CPtr first_free)
{
    s_untyped    = ut;
    s_cnode_root = cnode_root;
    s_next_slot  = first_free;

    /* Register taskman itself as pid 1, owner of root. */
    g_processes[0].in_use         = 1;
    g_processes[0].pid            = QSOE_PID_TASKMAN;
    g_processes[0].cnode          = cnode_root;
    g_processes[0].next_slot      = first_free;
    g_processes[0].tcb            = seL4_CapInitThreadTCB;
    g_processes[0].sc             = 0;
    g_processes[0].vspace         = seL4_CapInitThreadVSpace;
    g_processes[0].untyped_budget = 0;
    g_processes[0].workers_l1_pt  = 0;
    g_processes[0].workers_l0_pt  = 0;
    g_processes[0].next_tid       = 2;
    g_processes[0].parent_pid     = QSOE_PID_TASKMAN;  /* self-parent */
    g_processes[0].name[0] = 't'; g_processes[0].name[1] = 'a';
    g_processes[0].name[2] = 's'; g_processes[0].name[3] = 'k';
    g_processes[0].name[4] = 'm'; g_processes[0].name[5] = 'a';
    g_processes[0].name[6] = 'n'; g_processes[0].name[7] = '\0';
    g_processes[0].exit_state     = 0;
    g_processes[0].exit_status    = 0;
    g_processes[0].waiter_reply_slot = 0;
    /* taskman runs as root.  Every spawn inherits this cred until
     * setuid/setgid land. */
    g_processes[0].cred.ruid = 0;
    g_processes[0].cred.euid = 0;
    g_processes[0].cred.suid = 0;
    g_processes[0].cred.rgid = 0;
    g_processes[0].cred.egid = 0;
    g_processes[0].cred.sgid = 0;
    /* CWD starts at root; children inherit at spawn. */
    g_processes[0].cwd[0] = '/';
    g_processes[0].cwd[1] = 0;
    /* POSIX default umask — children inherit at spawn. */
    g_processes[0].umask = 022;
    /* ITIMER_REAL disarmed by default. */
    g_processes[0].itimer_expiry_ticks   = 0;
    g_processes[0].itimer_interval_ticks = 0;
    /* taskman itself owns no pp_ut blocks or object CNode -- it retypes
     * from the master pool directly and is never torn down. */
    g_processes[0].pput_count = 0;
    g_processes[0].objcnode = 0;
    g_processes[0].objcnode_next = 0;
    g_processes[0].fault_ep = 0;
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
        g_processes[i].sc             = 0;
        g_processes[i].vspace         = vspace;
        g_processes[i].untyped_budget = 0;
        g_processes[i].workers_l1_pt  = 0;
        g_processes[i].workers_l0_pt  = 0;
        g_processes[i].next_tid       = 2;
        g_processes[i].parent_pid     = QSOE_PID_TASKMAN;
        g_processes[i].exit_state     = 0;
        g_processes[i].exit_status    = 0;
        g_processes[i].waiter_reply_slot = 0;
        g_processes[i].signal_chid    = 0;
        g_processes[i].mmap_top       = QSOE_MMAP_BASE;
        g_processes[i].mmap_count      = 0;
        /* Recycle list starts empty: any frames the previous owner of
         * this slot parked were destroyed with its pp_ut blocks, so
         * those caps are stale -- never carry them across a reuse. */
        g_processes[i].mmap_free_count = 0;
        g_processes[i].devframe_count  = 0;
        g_processes[i].mprot_count     = 0;
        /* Inherit parent's cred at spawn.  main.c's PROCESS_CREATE
         * handler calls tm_process_set_parent (and we'd ideally
         * inherit cred from there); until QSOE has multi-user state
         * every process gets root.  Replace with parent->cred copy
         * once tm_process_set_parent is the inheritance point. */
        g_processes[i].cred.ruid = 0;
        g_processes[i].cred.euid = 0;
        g_processes[i].cred.suid = 0;
        g_processes[i].cred.rgid = 0;
        g_processes[i].cred.egid = 0;
        g_processes[i].cred.sgid = 0;
        /* CWD: start at "/"; tm_process_set_parent overwrites this
         * with a copy of the parent's cwd, matching POSIX inheritance. */
        g_processes[i].cwd[0] = '/';
        g_processes[i].cwd[1] = 0;
        g_processes[i].umask  = 022;
        g_processes[i].itimer_expiry_ticks   = 0;
        g_processes[i].itimer_interval_ticks = 0;
        g_processes[i].pput_count = 0;
        g_processes[i].objcnode = 0;
        g_processes[i].objcnode_next = 0;
        g_processes[i].fault_ep = 0;
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

/* Process table slot at `idx` (0..TM_MAX_PROCESSES-1), or NULL when out
 * of range.  Lets /proc enumerate the table without exposing the array. */
tm_process_t *tm_process_by_index(int idx)
{
    if (idx < 0 || idx >= TM_MAX_PROCESSES) return 0;
    return &g_processes[idx];
}

seL4_CPtr tm_process_find_frame(const tm_process_t *proc, unsigned long va)
{
    if (!proc) return 0;
    unsigned long key = va & ~(QSOE_MEGA_PAGE - 1);
    for (int i = 0; i < proc->mmap_count; ++i) {
        if (proc->mmap[i].va_page == key) return proc->mmap[i].frame;
    }
    return 0;
}

seL4_CPtr tm_process_alloc_slot(pid_t pid)
{
    tm_process_t *p = tm_process_lookup(pid);
    if (!p) return 0;
    /* For taskman, the global s_next_slot is the source of truth
     * (spawn.c bumps it directly).  Keep p->next_slot in sync. */
    if (p->pid == QSOE_PID_TASKMAN) {
        return s_next_slot++;
    }
    return p->next_slot++;
}

seL4_Uint8 cnode_depth_for(pid_t pid)
{
    return (pid == QSOE_PID_TASKMAN) ? TM_DEPTH_TASKMAN : TM_DEPTH_CHILD;
}

/* Free list of slots that were CNode_Deleted on Destroy paths, so
 * subsequent allocations reuse them instead of bumping s_next_slot
 * forever.  Required for the cap-leak smoke test to show 0 growth. */
#define TM_SLOT_FREE_LIST_MAX 256
static seL4_CPtr s_slot_free_list[TM_SLOT_FREE_LIST_MAX];
static int       s_slot_free_count = 0;

void taskman_free_slot(seL4_CPtr slot)
{
    if (slot == 0) return;
    if (s_slot_free_count < TM_SLOT_FREE_LIST_MAX) {
        s_slot_free_list[s_slot_free_count++] = slot;
    }
    /* If the free list is full we drop it — the slot stays unused,
     * which costs CSpace room but never corrupts state. */
}

seL4_CPtr taskman_alloc_empty_slot(void)
{
    if (s_slot_free_count > 0) {
        return s_slot_free_list[--s_slot_free_count];
    }
    return s_next_slot++;
}

/* ----------- per-process untyped (pp_ut) reclamation (v0.10) ----------- */

/* Reuse free-list of child-free pp_ut blocks. */
static seL4_CPtr s_pput_free[TM_PP_UT_FREE_MAX];
static int       s_pput_free_count;

/* RAM untypeds blocks are carved from, with a cursor that advances as
 * each fills.  Spans every RAM untyped (not just the largest), so the
 * whole board's memory is reachable. */
static seL4_CPtr s_ram_ut[TM_RAM_UT_MAX];
static int       s_ram_ut_count;
static int       s_ram_ut_cursor;

void tm_pput_pool_init(const seL4_CPtr *uts, int n)
{
    if (n > TM_RAM_UT_MAX) n = TM_RAM_UT_MAX;
    for (int i = 0; i < n; ++i) s_ram_ut[i] = uts[i];
    s_ram_ut_count  = n;
    s_ram_ut_cursor = 0;
}

/* Active per-process allocation context.  s_cur_pput == 0 means "draw
 * from the master pool" (the dispatcher's own reply churn + taskman-self
 * allocations).  When set, taskman_alloc_and_retype retypes from it and,
 * on exhaustion, grows by appending a fresh block to s_cur_pput_list. */
static seL4_CPtr  s_cur_pput;
static seL4_CPtr *s_cur_pput_list;
static int       *s_cur_pput_list_n;

/* Acquire a child-free pp_ut: reuse one from the free-list, else carve a
 * fresh TM_PP_UT_BITS block out of the master pool.  Returns 0 only if
 * the master pool itself is exhausted. */
static seL4_CPtr pp_ut_acquire(void)
{
    if (s_pput_free_count > 0)
        return s_pput_free[--s_pput_free_count];
    /* Carve a fresh block from the RAM pool, advancing past untypeds
     * that no longer have a full block left. */
    while (s_ram_ut_cursor < s_ram_ut_count) {
        seL4_CPtr slot = taskman_alloc_empty_slot();
        if (!slot) return 0;
        seL4_Word err = qsoe_untyped_retype(s_ram_ut[s_ram_ut_cursor],
                                            seL4_UntypedObject, TM_PP_UT_BITS,
                                            s_cnode_root, 0, 0, slot, 1);
        if (err == 0) return slot;
        taskman_free_slot(slot);
        s_ram_ut_cursor++;            /* this untyped is full -> next */
    }
    return 0;                         /* whole board exhausted */
}

/* Return a pp_ut for reuse: Revoke it (destroying every object still
 * retyped from it, leaving it child-free so seL4 auto-resets its free
 * index on the next retype) and push it onto the free-list.  If the
 * free-list is full, drop the block (delete cap + recycle slot); its RAM
 * stays carved from the master pool, but that is bounded by
 * TM_PP_UT_FREE_MAX. */
static void pp_ut_release(seL4_CPtr ut)
{
    if (!ut) return;
    qsoe_cnode_revoke(s_cnode_root, ut, TM_DEPTH_TASKMAN);
    if (s_pput_free_count < TM_PP_UT_FREE_MAX) {
        s_pput_free[s_pput_free_count++] = ut;
    } else {
        qsoe_cnode_delete(s_cnode_root, ut, TM_DEPTH_TASKMAN);
        taskman_free_slot(ut);
    }
}

int tm_pput_spawn_begin(seL4_CPtr *staging, int *staging_n)
{
    seL4_CPtr first = pp_ut_acquire();
    if (!first) return -ENOMEM;
    staging[0]        = first;
    *staging_n        = 1;
    s_cur_pput        = first;
    s_cur_pput_list   = staging;
    s_cur_pput_list_n = staging_n;
    return 0;
}

void tm_pput_proc_begin(tm_process_t *p)
{
    if (!p || p->pput_count <= 0) return;   /* no blocks -> master pool */
    s_cur_pput        = p->pput[p->pput_count - 1];
    s_cur_pput_list   = p->pput;
    s_cur_pput_list_n = &p->pput_count;
}

void tm_pput_end(void)
{
    s_cur_pput        = 0;
    s_cur_pput_list   = 0;
    s_cur_pput_list_n = 0;
}

void tm_pput_release_list(seL4_CPtr *list, int n)
{
    for (int i = 0; i < n; ++i)
        pp_ut_release(list[i]);
}

seL4_CPtr taskman_alloc_and_retype(seL4_Word type, seL4_Word size_bits)
{
    seL4_CPtr slot = taskman_alloc_empty_slot();
    if (!slot) return 0;
    seL4_CPtr ut  = s_cur_pput ? s_cur_pput : s_untyped;
    seL4_Word err = qsoe_untyped_retype(ut, type, size_bits,
                                        s_cnode_root, 0, 0, slot, 1);
    /* pp_ut exhausted mid-spawn/mmap: grow by another block and retry. */
    if (err != 0 && s_cur_pput && s_cur_pput_list &&
        *s_cur_pput_list_n < TM_PP_UT_PER_PROC) {
        seL4_CPtr nut = pp_ut_acquire();
        if (nut) {
            s_cur_pput_list[(*s_cur_pput_list_n)++] = nut;
            s_cur_pput = nut;
            err = qsoe_untyped_retype(nut, type, size_bits,
                                      s_cnode_root, 0, 0, slot, 1);
        }
    }
    if (err != 0) {
        taskman_free_slot(slot);
        return 0;
    }
    return slot;
}

/* ----------- MCS scheduling contexts + reply objects (v0.10) ----------- */

/* Round-robin / best-effort budget: budget == period makes a thread
 * simply runnable whenever it is the highest-priority ready thread —
 * the closest MCS analogue to the non-MCS scheduler QSOE/L ran before
 * v0.10.  1 ms quantum. */
#define TM_SC_PERIOD_US  1000u
#define TM_SC_BUDGET_US  TM_SC_PERIOD_US

/* Retype a scheduling context and program it (budget==period) on the
 * given core's SchedControl.  Returns the SC cap, or 0 on failure.
 * SchedContext is variable-size, so the retype size_bits MUST be
 * >= seL4_MinSchedContextBits. */
seL4_CPtr tm_sched_context_create(unsigned core)
{
    seL4_CPtr sc = taskman_alloc_and_retype(seL4_SchedContextObject,
                                            seL4_MinSchedContextBits);
    if (!sc) return 0;
    seL4_Word err = qsoe_sched_control_configure(tm_sched_control_for_core(core),
                                                 sc,
                                                 TM_SC_BUDGET_US, TM_SC_PERIOD_US,
                                                 0, 0, 0);
    if (err != 0) {
        qsoe_cnode_delete(s_cnode_root, sc, TM_DEPTH_TASKMAN);
        taskman_free_slot(sc);
        return 0;
    }
    return sc;
}

/* Retype a fresh reply object into a freshly-allocated taskman slot. */
seL4_CPtr tm_reply_object_create(void)
{
    return taskman_alloc_and_retype(seL4_ReplyObject, 0);
}

/* One-time: hand the dispatcher its active reply object.  Must run
 * before the first Recv (see main.c). */
int tm_reply_init(void)
{
    s_active_reply = tm_reply_object_create();
    return s_active_reply ? 0 : -ENOMEM;
}

seL4_CPtr tm_active_reply(void) { return s_active_reply; }

/* Park the current caller's reply: move the dispatcher's active reply
 * cap into a stash slot (the caller stays blocked, tracked by that
 * cap), then replenish a fresh reply object for the next Recv.  Returns
 * the stash slot, or 0 on failure.  MCS replacement for the retired
 * qsoe_cnode_save_caller. */
seL4_CPtr tm_reply_park(void)
{
    seL4_CPtr slot = taskman_alloc_empty_slot();
    if (!slot) return 0;
    if (qsoe_cnode_move(s_cnode_root, slot, TM_DEPTH_TASKMAN,
                        s_cnode_root, s_active_reply, TM_DEPTH_TASKMAN) != 0) {
        taskman_free_slot(slot);
        return 0;
    }
    /* s_active_reply is now empty; retype a new reply object into it. */
    if (qsoe_untyped_retype(s_untyped, seL4_ReplyObject, 0,
                            s_cnode_root, 0, 0, s_active_reply, 1) != 0) {
        /* Move the caller's reply back so it isn't stranded, then fail. */
        qsoe_cnode_move(s_cnode_root, s_active_reply, TM_DEPTH_TASKMAN,
                        s_cnode_root, slot, TM_DEPTH_TASKMAN);
        taskman_free_slot(slot);
        return 0;
    }
    return slot;
}

/* Deliver a deferred reply parked in `slot`: Send to the stashed reply
 * object (unblocks the original caller), then delete the reply object
 * and recycle the slot.  An MCS reply object is not self-cleared by the
 * Send, so the delete keeps the cap-leak smoke test flat. */
void tm_reply_deliver(seL4_CPtr slot, seL4_MessageInfo_t tag,
                      seL4_Word mr0, seL4_Word mr1, seL4_Word mr2, seL4_Word mr3)
{
    if (slot == 0) return;
    qsoe_sys_send(slot, tag, mr0, mr1, mr2, mr3);
    qsoe_cnode_delete(s_cnode_root, slot, TM_DEPTH_TASKMAN);
    taskman_free_slot(slot);
}

/* Discard a parked reply WITHOUT answering it — for cleanup when the
 * blocked caller is gone (e.g. its process was terminated).  Deletes
 * the stashed reply object and recycles the slot. */
void tm_reply_drop(seL4_CPtr slot)
{
    if (slot == 0) return;
    qsoe_cnode_delete(s_cnode_root, slot, TM_DEPTH_TASKMAN);
    taskman_free_slot(slot);
}

/* ----------- v0.6.1 procmgr_detach / waitpid plumbing ----------- */

int tm_process_set_parent(pid_t child, pid_t parent)
{
    tm_process_t *p = tm_process_lookup(child);
    if (!p) return -ESRCH;
    p->parent_pid = parent;
    /* v0.7: inherit cred AND cwd from parent here.  Single-user
     * system means parent is root → child is root, but the copy
     * is structural so setuid/setgid and chdir propagation across
     * spawn keeps working as features land. */
    tm_process_t *par = tm_process_lookup(parent);
    if (par) {
        p->cred  = par->cred;
        p->umask = par->umask;
        for (unsigned i = 0; i < sizeof p->cwd; ++i) {
            p->cwd[i] = par->cwd[i];
        }
    }
    return 0;
}

static void deliver_waiter_reply(seL4_CPtr slot, seL4_Word label, int status)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(label, 0, 0, 1);
    tm_reply_deliver(slot, tag, (seL4_Word)(unsigned)status, 0, 0, 0);
}

int tm_process_detach(pid_t pid, int status)
{
    tm_process_t *p = tm_process_lookup(pid);
    if (!p) return -ESRCH;
    if (p->exit_state != 0) return 0;

    p->exit_state  = 1;
    p->exit_status = status;

    if (p->waiter_reply_slot != 0) {
        seL4_CPtr slot = p->waiter_reply_slot;
        p->waiter_reply_slot = 0;
        deliver_waiter_reply(slot, /*label=*/0, status);
    }

    p->parent_pid = QSOE_PID_TASKMAN;
    return 0;
}

int tm_process_waitpid(pid_t waiter, pid_t child,
                       int *out_status, int *out_parked)
{
    *out_parked = 0;
    tm_process_t *c = tm_process_lookup(child);
    if (!c) return -ECHILD;
    if (c->parent_pid != waiter) return -ECHILD;

    if (c->exit_state != 0) {
        *out_status = c->exit_status;
        return 0;
    }

    seL4_CPtr slot = tm_reply_park();
    if (slot == 0) {
        return -ENOMEM;
    }
    c->waiter_reply_slot = slot;
    *out_parked = 1;
    return 0;
}

/* ----------- v0.7 self info: pid + ppid + cred ----------- */

int tm_proc_self_info(pid_t caller_pid,
                      pid_t *out_pid, pid_t *out_ppid,
                      tm_cred_t *out_cred)
{
    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;
    if (out_pid)  *out_pid  = p->pid;
    if (out_ppid) *out_ppid = p->parent_pid;
    if (out_cred) *out_cred = p->cred;
    return 0;
}

/* ----------- v0.7 cwd ----------- */

int tm_chdir(pid_t caller_pid, unsigned path_len)
{
    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;
    if (path_len == 0 || path_len >= sizeof p->cwd) return -ENAMETOOLONG;

    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    /* v0.7 accepts absolute paths only.  Relative paths need full
     * cwd-relative resolution which lands when we have a real fs. */
    if (src[0] != '/') return -EINVAL;

    /* Copy into the process's cwd, NUL-terminating.  No existence /
     * is-a-directory check yet: cpiofs is too flat to know about
     * directories, and pathmgr's "/" catch-all would lie if we asked.
     * Once a real fs lands, validate here. */
    for (unsigned i = 0; i < path_len; ++i) p->cwd[i] = (char)src[i];
    p->cwd[path_len] = 0;
    return 0;
}

int tm_getcwd(pid_t caller_pid, unsigned *out_len)
{
    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;

    unsigned len = 0;
    while (p->cwd[len] != 0 && len < sizeof p->cwd) ++len;

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < len; ++i) dst[i] = (unsigned char)p->cwd[i];
    *out_len = len;
    return 0;
}

/* ----------- v0.7 dup-cap (backs dup2 / F_DUPFD) ----------- */

int tm_dup_cap(pid_t caller_pid, seL4_CPtr src_slot, seL4_CPtr dest_slot)
{
    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;
    if (src_slot == 0 || dest_slot == 0) return -EINVAL;
    if (src_slot == dest_slot) return 0;
    seL4_Uint8 depth = cnode_depth_for(caller_pid);
    if (qsoe_cnode_copy(p->cnode, dest_slot, depth,
                        p->cnode, src_slot, depth,
                        QSOE_RIGHTS_ALL) != 0) {
        return -EBADF;
    }
    /* Mirror the cap copy in the connection registry so each fd has
     * an independent row.  Without this, the dup'd fd would share a
     * single registry row with the source fd, and closing the source
     * (TM_REQ_DETACH_CAP) would drop the row out from under the
     * surviving fd — reads then fail EBADF. */
    int crc = tm_connection_clone_for_dup(caller_pid, src_slot, dest_slot);
    if (crc != 0) {
        (void)qsoe_cnode_delete(p->cnode, dest_slot, depth);
        return crc;
    }
    return 0;
}

/* ----------- v0.7 umask ----------- */

int tm_umask(pid_t caller_pid, int set, unsigned *out_old)
{
    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;
    *out_old = p->umask;
    if (set >= 0) p->umask = (unsigned)set & 0777u;
    return 0;
}

int tm_set_cred(pid_t caller_pid,
                unsigned ruid_new, unsigned euid_new, unsigned suid_new,
                unsigned rgid_new, unsigned egid_new, unsigned sgid_new)
{
    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;
    /* 0xFFFFFFFF = "leave alone".  v0.8 verifies p->cred.euid == 0
     * (or that the new values match current ones) before applying;
     * v0.7 is single-user-root so every call succeeds. */
    if (ruid_new != 0xFFFFFFFFu) p->cred.ruid = (uid_t)ruid_new;
    if (euid_new != 0xFFFFFFFFu) p->cred.euid = (uid_t)euid_new;
    if (suid_new != 0xFFFFFFFFu) p->cred.suid = (uid_t)suid_new;
    if (rgid_new != 0xFFFFFFFFu) p->cred.rgid = (gid_t)rgid_new;
    if (egid_new != 0xFFFFFFFFu) p->cred.egid = (gid_t)egid_new;
    if (sgid_new != 0xFFFFFFFFu) p->cred.sgid = (gid_t)sgid_new;
    return 0;
}

/* ----------- v0.13 scheduling (SchedSet / SchedGet) ----------- */

/* Resolve (pid, tid) to the target thread's TCB cap plus pointers to its
 * tracked scheduling state.  pid==0 -> caller; tid<=1 -> the process's main
 * thread (p->tcb / p->sched_*), a higher tid -> a TM_REQ_THREAD_ALLOC worker
 * (tm_thread_find).  Returns 0 with the out-params filled, or -ESRCH. */
static int sched_resolve(pid_t caller_pid, pid_t pid, int tid,
                         seL4_CPtr *out_tcb, int **out_prio, int **out_policy)
{
    if (pid == 0) pid = caller_pid;
    if (tid <= 1) {
        tm_process_t *p = tm_process_lookup(pid);
        if (!p) return -ESRCH;
        *out_tcb    = p->tcb;
        *out_prio   = &p->sched_prio;
        *out_policy = &p->sched_policy;
        return 0;
    }
    tm_thread_t *t = tm_thread_find(pid, tid);
    if (!t) return -ESRCH;
    *out_tcb    = t->tcb_master;
    *out_prio   = &t->sched_prio;
    *out_policy = &t->sched_policy;
    return 0;
}

int tm_sched_set(pid_t caller_pid, pid_t pid, int tid, int policy, int prio)
{
    /* SCHED_OTHER(0)..SCHED_RR(TM_SCHED_RR) is the whole policy range. */
    if (policy < 0 || policy > TM_SCHED_RR) return -EINVAL;
    if (prio < TM_SCHED_PRIO_MIN || prio > TM_SCHED_PRIO_MAX) return -EINVAL;

    seL4_CPtr tcb; int *cur_prio; int *cur_policy;
    int rc = sched_resolve(caller_pid, pid, tid, &tcb, &cur_prio, &cur_policy);
    if (rc) return rc;

    /* TCB_SetPriority leaves the round-robin SC untouched -- only the
     * priority changes.  seL4 caps the new value at the authority TCB's
     * MCP; taskman's InitThread authority (255) sits above
     * TM_SCHED_PRIO_MAX, so the bound check above is the real limiter. */
    seL4_Word err = qsoe_tcb_set_priority(tcb, seL4_CapInitThreadTCB,
                                          (seL4_Word)prio);
    if (err) return -EINVAL;

    *cur_prio   = prio;
    *cur_policy = policy;
    return 0;
}

int tm_sched_get(pid_t caller_pid, pid_t pid, int tid,
                 int *out_policy, int *out_prio)
{
    seL4_CPtr tcb; int *cur_prio; int *cur_policy;
    int rc = sched_resolve(caller_pid, pid, tid, &tcb, &cur_prio, &cur_policy);
    if (rc) return rc;
    if (out_prio)   *out_prio   = *cur_prio;
    if (out_policy) *out_policy = *cur_policy;
    return 0;
}

/* ----------- v0.4.1 process termination ----------- */

int tm_process_terminate(pid_t target, int status)
{
    if (target == QSOE_PID_TASKMAN) return -EINVAL;
    tm_process_t *p = tm_process_lookup(target);
    if (!p) return -ESRCH;

    if (p->exit_state == 0) {
        p->exit_state  = 2;
        p->exit_status = status;
        if (p->waiter_reply_slot != 0) {
            seL4_CPtr slot = p->waiter_reply_slot;
            p->waiter_reply_slot = 0;
            deliver_waiter_reply(slot, /*label=*/0, status);
        }
    }

    tm_thread_t  *gthreads  = tm_threads_array();
    tm_channel_t *gchannels = tm_channels_array();

    /* 1. Worker threads of this pid. */
    for (int i = 0; i < TM_MAX_THREADS; ++i) {
        if (gthreads[i].in_use && gthreads[i].pid == target) {
            qsoe_cnode_revoke(s_cnode_root, gthreads[i].tcb_master, TM_DEPTH_TASKMAN);
            qsoe_cnode_delete(s_cnode_root, gthreads[i].tcb_master, TM_DEPTH_TASKMAN);
            taskman_free_slot(gthreads[i].tcb_master);
            qsoe_cnode_revoke(s_cnode_root, gthreads[i].ntfn_master, TM_DEPTH_TASKMAN);
            qsoe_cnode_delete(s_cnode_root, gthreads[i].ntfn_master, TM_DEPTH_TASKMAN);
            taskman_free_slot(gthreads[i].ntfn_master);
            /* SC object dies with Revoke(pput); reclaim its root slot.
             * TCB already deleted above, so the SC is unbound. */
            if (gthreads[i].sc) {
                qsoe_cnode_delete(s_cnode_root, gthreads[i].sc, TM_DEPTH_TASKMAN);
                taskman_free_slot(gthreads[i].sc);
                gthreads[i].sc = 0;
            }
            gthreads[i].in_use = 0;
        }
    }

    /* 2. Channels owned by this pid. */
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (gchannels[i].in_use && gchannels[i].owner_pid == target) {
            if (gchannels[i].ntfn_master) {
                qsoe_cnode_revoke(s_cnode_root, gchannels[i].ntfn_master, TM_DEPTH_TASKMAN);
                qsoe_cnode_delete(s_cnode_root, gchannels[i].ntfn_master, TM_DEPTH_TASKMAN);
                taskman_free_slot(gchannels[i].ntfn_master);
                /* ntfn_sig is a minted child of ntfn_master, so the
                 * revoke above already destroyed its cap -- but its root
                 * slot must still be returned, else every channel leaks
                 * one slot. */
                if (gchannels[i].ntfn_sig)
                    taskman_free_slot(gchannels[i].ntfn_sig);
                gchannels[i].ntfn_master = 0;
                gchannels[i].ntfn_sig = 0;
            }
            qsoe_cnode_revoke(s_cnode_root, gchannels[i].master, TM_DEPTH_TASKMAN);
            qsoe_cnode_delete(s_cnode_root, gchannels[i].master, TM_DEPTH_TASKMAN);
            taskman_free_slot(gchannels[i].master);
            gchannels[i].in_use = 0;
        }
    }

    /* 3. Connections this pid held as a client.  Walk via the
     *    public API since g_connections is owned by connect.c. */
    for (int i = 0; i < TM_MAX_CONNECTIONS; ++i) {
        /* No iterator API yet; use the helper that finds-by-pid+slot
         * one at a time would be O(n^2).  Since channel teardown
         * above already invalidates the connections that pointed at
         * this pid's channels, the remaining work — clearing client
         * entries — happens lazily on next lookup.  See connect.c
         * tm_connections_drop_for_pid for a sweep helper that we'll
         * add when termination cost becomes visible. */
        (void)i;
    }
    extern void tm_connections_drop_for_pid(pid_t pid);
    tm_connections_drop_for_pid(target);

    /* 4. Main TCB. */
    qsoe_cnode_revoke(s_cnode_root, p->tcb, TM_DEPTH_TASKMAN);
    qsoe_cnode_delete(s_cnode_root, p->tcb, TM_DEPTH_TASKMAN);
    taskman_free_slot(p->tcb);

    /* 4a. Main-thread SC.  Object dies with Revoke(pput); reclaim its
     *     root slot now that the TCB it was bound to is gone. */
    if (p->sc) {
        qsoe_cnode_delete(s_cnode_root, p->sc, TM_DEPTH_TASKMAN);
        taskman_free_slot(p->sc);
        p->sc = 0;
    }

    /* 4b. Fault-handler cap -- the TCB referenced it; safe to drop now. */
    if (p->fault_ep) {
        qsoe_cnode_delete(s_cnode_root, p->fault_ep, TM_DEPTH_TASKMAN);
        taskman_free_slot(p->fault_ep);
        p->fault_ep = 0;
    }

    /* 5. VSpace. */
    qsoe_cnode_revoke(s_cnode_root, p->vspace, TM_DEPTH_TASKMAN);
    qsoe_cnode_delete(s_cnode_root, p->vspace, TM_DEPTH_TASKMAN);
    taskman_free_slot(p->vspace);

    /* 6. CNode. */
    qsoe_cnode_revoke(s_cnode_root, p->cnode, TM_DEPTH_TASKMAN);
    qsoe_cnode_delete(s_cnode_root, p->cnode, TM_DEPTH_TASKMAN);
    taskman_free_slot(p->cnode);

    /* 7. Workers L0/L1 PTs if allocated. */
    if (p->workers_l0_pt) {
        qsoe_cnode_delete(s_cnode_root, p->workers_l0_pt, TM_DEPTH_TASKMAN);
        taskman_free_slot(p->workers_l0_pt);
    }
    if (p->workers_l1_pt) {
        qsoe_cnode_delete(s_cnode_root, p->workers_l1_pt, TM_DEPTH_TASKMAN);
        taskman_free_slot(p->workers_l1_pt);
    }

    /* 8. Untyped budget. */
    if (p->untyped_budget) {
        qsoe_cnode_revoke(s_cnode_root, p->untyped_budget, TM_DEPTH_TASKMAN);
        qsoe_cnode_delete(s_cnode_root, p->untyped_budget, TM_DEPTH_TASKMAN);
        taskman_free_slot(p->untyped_budget);
    }

    /* 8b. mmap megapage frame caps -- both the live mappings and the
     *     parked recycle list.  Their objects die with Revoke(pput)
     *     below; recycle the root-CNode slots they occupied. */
    for (int i = 0; i < p->mmap_count; ++i) {
        if (p->mmap[i].frame) taskman_free_slot(p->mmap[i].frame);
    }
    p->mmap_count = 0;
    for (int i = 0; i < p->mmap_free_count; ++i) {
        if (p->mmap_free[i]) taskman_free_slot(p->mmap_free[i]);
    }
    p->mmap_free_count = 0;

    /* 8c. MAP_PHYS device-frame copies.  The VSpace revoke above already
     *     unmapped them; delete the copy cap (the shared frame survives
     *     in the device-map registry) and recycle the root slot. */
    for (int i = 0; i < p->devframe_count; ++i) {
        if (p->devframes[i]) {
            qsoe_cnode_delete(s_cnode_root, p->devframes[i], TM_DEPTH_TASKMAN);
            taskman_free_slot(p->devframes[i]);
        }
    }
    p->devframe_count = 0;

    /* 8d. RELRO frame caps kept invokeable for mprotect.  Their objects
     *     are pp_ut children destroyed by Revoke(pput) below (same as the
     *     mmap megapages); only the root-CNode slots need reclaiming. */
    for (int i = 0; i < p->mprot_count; ++i) {
        if (p->mprot[i].frame) taskman_free_slot(p->mprot[i].frame);
    }
    p->mprot_count = 0;

    /* 9. Per-process untyped blocks.  Revoke each (destroying every
     *    object still retyped from it -- image frames, page tables,
     *    mmap megapages, the child untyped above) and return it to the
     *    reuse free-list.  This is what actually hands the process's RAM
     *    back; the individual deletes above only drop taskman's caps. */
    tm_pput_release_list(p->pput, p->pput_count);
    p->pput_count = 0;

    /* 10. The object CNode itself is a pp_ut child, so Revoke above
     *     already destroyed it (and every image-frame cap it held);
     *     just recycle its root-CNode slot. */
    if (p->objcnode) {
        taskman_free_slot(p->objcnode);
        p->objcnode = 0;
        p->objcnode_next = 0;
    }

    p->in_use = 0;
    tm_pid_free(target);
    return 0;
}

void tm_handle_fault(pid_t pid, unsigned fault_type)
{
    tm_process_t *p = tm_process_lookup(pid);
    const char *name = (p && p->name[0]) ? p->name : "?";
    /* Every fatal U-mode fault terminates the process; SIGSEGV covers
     * the common bad/NULL-pointer VM fault.  (A finer fault-type ->
     * signal mapping -- SIGILL, SIGBUS -- can refine this later.) */
    tm_warn("pid %d (%s) faulted (seL4 fault type %u) -- terminating (SIGSEGV)",
            (int)pid, name, fault_type);
    (void) tm_process_terminate(pid, TM_SIG_SEGV);
}
