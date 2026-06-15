/*
 * proc/thread.c — thread allocator: retypes TCB + Notification + IPC
 * frame + N stack frames, maps them into the caller's VSpace, and
 * copies the master caps into the caller's CSpace.  Backs
 * TM_REQ_THREAD_ALLOC.
 *
 * Split out of v0.6.4's server.c.
 */

#include "proc.h"
#include "../qsoe_invoke.h"

static tm_thread_t g_threads[TM_MAX_THREADS];

tm_thread_t *tm_threads_array(void) { return g_threads; }

tm_thread_t *tm_thread_find(pid_t pid, int tid)
{
    for (int i = 0; i < TM_MAX_THREADS; ++i) {
        if (g_threads[i].in_use &&
            g_threads[i].pid == pid && g_threads[i].tid == tid)
            return &g_threads[i];
    }
    return 0;
}

static int thread_alloc_slot_idx(void)
{
    for (int i = 0; i < TM_MAX_THREADS; ++i) {
        if (!g_threads[i].in_use) return i;
    }
    return -1;
}

/* Ensure the caller's VSpace has page tables for the worker thread
 * region at [0x40000000, 0x40200000).
 *
 * The image stays ≤ 1 GiB ([[project_image_size_cap]]) so it lives in
 * L2[0] and never shares an L1 PT with the worker region in L2[1].
 *
 * However libc.so (DL_LIBC_LOAD_VA=0x60000000) and rtld
 * (DL_RTLD_LOAD_VA=0x70000000) DO live in L2[1] — the spawn path
 * already installs that L1 PT and stashes its cap as workers_l1_pt
 * so we don't try to install a second L1 here and trip seL4's
 * "All objects mapped at this address".  Static spawns (no dyn-link)
 * leave workers_l1_pt NULL and we install both PTs ourselves. */
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
                    seL4_CPtr *out_ntfn_slot,
                    seL4_CPtr *out_reply_slot)
{
    if (caller_pid == QSOE_PID_TASKMAN) {
        return -ENOSYS;
    }
    tm_process_t *p = tm_process_lookup(caller_pid);
    if (!p) return -ESRCH;
    if (p->next_tid >= TM_MAX_TID_PER_PROC + 1) return -ENOMEM;
    if (stack_pages == 0 || stack_pages > 16) return -EINVAL;

    int gidx = thread_alloc_slot_idx();
    if (gidx < 0) return -ENOMEM;

    int new_tid = p->next_tid++;

    int pterr = ensure_workers_pts(p);
    if (pterr) return pterr;

    seL4_CPtr tcb       = taskman_alloc_and_retype(seL4_TCBObject, 0);
    if (!tcb) return -ENOMEM;
    seL4_CPtr ntfn      = taskman_alloc_and_retype(seL4_NotificationObject, 0);
    if (!ntfn) return -ENOMEM;
    seL4_CPtr ipc_frame = taskman_alloc_and_retype(seL4_RISCV_4K_Page, 0);
    if (!ipc_frame) return -ENOMEM;

    seL4_Word err = qsoe_riscv_page_map(ipc_frame, p->vspace, ipc_vaddr,
                                         QSOE_RIGHTS_ALL,
                                         QSOE_VM_ATTR_DEFAULT);
    if (err) return -ENOMEM;

    for (unsigned i = 0; i < stack_pages; ++i) {
        seL4_CPtr f = taskman_alloc_and_retype(seL4_RISCV_4K_Page, 0);
        if (!f) return -ENOMEM;
        unsigned long va = stack_top_vaddr - (unsigned long)(i + 1) * 4096UL;
        err = qsoe_riscv_page_map(f, p->vspace, va,
                                   QSOE_RIGHTS_ALL,
                                   QSOE_VM_ATTR_DEFAULT);
        if (err) return -ENOMEM;
    }

    err = qsoe_tcb_configure(tcb,
                              p->cnode, 52UL,
                              p->vspace, 0,
                              ipc_vaddr, ipc_frame);
    if (err) return -ENOMEM;

    /* MCS: bind a scheduling context (placed on the requested core —
     * that placement is how MCS expresses affinity, TCB_SetAffinity
     * being retired) so the worker can run, and set its priority in the
     * same SetSchedParams invocation. */
    seL4_CPtr sc = tm_sched_context_create(affinity);
    if (!sc) return -ENOMEM;
    /* Worker shares the process's fault handler (the badge identifies the
     * process, so a worker fault terminates the whole process -- correct).
     * No extra cap: reuse p->fault_ep minted at spawn. */
    err = qsoe_tcb_set_sched_params(tcb, seL4_CapInitThreadTCB,
                                    /*mcp=*/prio, /*prio=*/prio,
                                    sc, p->fault_ep);
    if (err) return -ENOMEM;

    seL4_CPtr child_tcb_slot  = tm_process_alloc_slot(caller_pid);
    seL4_CPtr child_ntfn_slot = tm_process_alloc_slot(caller_pid);
    seL4_Uint8 ddepth = cnode_depth_for(caller_pid);

    /* MCS: give the worker its own reply object (a reply object binds
     * one caller at a time, so threads cannot share one).  Retype it
     * straight into the worker's CSpace and hand the slot back so the
     * thread's libc startup can record it for MsgReceive/MsgReply. */
    seL4_CPtr child_reply_slot = tm_process_alloc_slot(caller_pid);
    if (qsoe_untyped_retype(s_untyped, seL4_ReplyObject, 0,
                            p->cnode, 0, 0, child_reply_slot, 1) != 0) {
        return -ENOMEM;
    }

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
    g_threads[gidx].sc             = sc;
    g_threads[gidx].sched_prio     = (int)prio;   /* tracked for SchedGet */
    g_threads[gidx].sched_policy   = TM_SCHED_RR;
    g_threads[gidx].name[0]        = '\0';   /* unnamed until tagged */

    *out_tid        = new_tid;
    *out_tcb_slot   = child_tcb_slot;
    *out_ntfn_slot  = child_ntfn_slot;
    *out_reply_slot = child_reply_slot;
    return 0;
}
