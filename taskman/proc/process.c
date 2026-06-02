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
#include "../tm_log.h"
#include <qsoe/slots.h>
#include <cpio.h>

static tm_process_t g_processes[TM_MAX_PROCESSES];

/* Shared globals — referenced by channel.c, connect.c, thread.c
 * (extern in proc.h).  Kept non-static so spawn.c (one dir up) can
 * still bump s_next_slot during boot. */
seL4_CPtr s_untyped;
seL4_CPtr s_cnode_root;
seL4_CPtr s_next_slot;

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

    int sr = tm_spawn(elf, elf_size, new_pid, s_primary_ep,
                       argc, argv, envc, envp, basename);
    if (sr) {
        tm_pid_free(new_pid);
        return sr;
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
    g_processes[0].vspace         = seL4_CapInitThreadVSpace;
    g_processes[0].untyped_budget = 0;
    g_processes[0].workers_l1_pt  = 0;
    g_processes[0].workers_l0_pt  = 0;
    g_processes[0].next_tid       = 2;
    g_processes[0].parent_pid     = QSOE_PID_TASKMAN;  /* self-parent */
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

seL4_CPtr taskman_alloc_and_retype(seL4_Word type, seL4_Word size_bits)
{
    seL4_CPtr slot = taskman_alloc_empty_slot();
    seL4_Word err = qsoe_untyped_retype(s_untyped, type, size_bits,
                                         s_cnode_root, 0, 0, slot, 1);
    if (err != 0) {
        taskman_free_slot(slot);
        return 0;
    }
    return slot;
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
    qsoe_sys_send(slot, tag, (seL4_Word)(unsigned)status, 0, 0, 0);
    taskman_free_slot(slot);
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

    seL4_CPtr slot = taskman_alloc_empty_slot();
    if (qsoe_cnode_save_caller(s_cnode_root, slot, TM_DEPTH_TASKMAN) != 0) {
        taskman_free_slot(slot);
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
            qsoe_cnode_revoke(s_cnode_root, gthreads[i].ntfn_master, TM_DEPTH_TASKMAN);
            qsoe_cnode_delete(s_cnode_root, gthreads[i].ntfn_master, TM_DEPTH_TASKMAN);
            gthreads[i].in_use = 0;
        }
    }

    /* 2. Channels owned by this pid. */
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (gchannels[i].in_use && gchannels[i].owner_pid == target) {
            if (gchannels[i].ntfn_master) {
                qsoe_cnode_revoke(s_cnode_root, gchannels[i].ntfn_master, TM_DEPTH_TASKMAN);
                qsoe_cnode_delete(s_cnode_root, gchannels[i].ntfn_master, TM_DEPTH_TASKMAN);
                gchannels[i].ntfn_master = 0;
                gchannels[i].ntfn_sig = 0;
            }
            qsoe_cnode_revoke(s_cnode_root, gchannels[i].master, TM_DEPTH_TASKMAN);
            qsoe_cnode_delete(s_cnode_root, gchannels[i].master, TM_DEPTH_TASKMAN);
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

    /* 5. VSpace. */
    qsoe_cnode_revoke(s_cnode_root, p->vspace, TM_DEPTH_TASKMAN);
    qsoe_cnode_delete(s_cnode_root, p->vspace, TM_DEPTH_TASKMAN);

    /* 6. CNode. */
    qsoe_cnode_revoke(s_cnode_root, p->cnode, TM_DEPTH_TASKMAN);
    qsoe_cnode_delete(s_cnode_root, p->cnode, TM_DEPTH_TASKMAN);

    /* 7. Workers L0/L1 PTs if allocated. */
    if (p->workers_l0_pt) {
        qsoe_cnode_delete(s_cnode_root, p->workers_l0_pt, TM_DEPTH_TASKMAN);
    }
    if (p->workers_l1_pt) {
        qsoe_cnode_delete(s_cnode_root, p->workers_l1_pt, TM_DEPTH_TASKMAN);
    }

    /* 8. Untyped budget. */
    if (p->untyped_budget) {
        qsoe_cnode_revoke(s_cnode_root, p->untyped_budget, TM_DEPTH_TASKMAN);
        qsoe_cnode_delete(s_cnode_root, p->untyped_budget, TM_DEPTH_TASKMAN);
    }

    p->in_use = 0;
    tm_pid_free(target);
    return 0;
}
