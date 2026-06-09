/*
 * qsoe_invoke.h — hand-rolled seL4 capability invocations for taskman.
 *
 * Mirrors the marshalling done by libsel4's generated stubs (verified against
 * `python3 syscall_stub_gen.py -a riscv64 ...` output, methods we use:
 *   UntypedRetype, CNodeRevoke, CNodeDelete, CNodeCopy, CNodeMint).
 *
 * The IPC buffer pointer lives in the current thread's qsoe_tcb_t
 * (see <qsoe/tls.h>) — the crt0 plants &qsoe_main_tcb in tp before
 * any call below, and qsoe_libc_init() then writes the buffer
 * address into that struct.
 */
#ifndef QSOE_INVOKE_H
#define QSOE_INVOKE_H

#include "sel4_types.h"
#include <qsoe/tls.h>   /* qsoe_ipcbuf macro */

/* Low-level ecall: dest in a0, info in a1, first four MRs in a2-a5,
 * syscall number in a7. Returns the reply MessageInfo word. */
static inline seL4_MessageInfo_t
qsoe_sys_call(seL4_CPtr dest, seL4_MessageInfo_t info,
              seL4_Word *mr0, seL4_Word *mr1, seL4_Word *mr2, seL4_Word *mr3)
{
    register seL4_Word a0 __asm__("a0") = dest;
    register seL4_Word a1 __asm__("a1") = info.words[0];
    register seL4_Word a2 __asm__("a2") = *mr0;
    register seL4_Word a3 __asm__("a3") = *mr1;
    register seL4_Word a4 __asm__("a4") = *mr2;
    register seL4_Word a5 __asm__("a5") = *mr3;
    register seL4_Word a7 __asm__("a7") = (seL4_Word)SYS_Call;
    __asm__ volatile("ecall"
                 : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5)
                 : "r"(a7)
                 : "memory");
    *mr0 = a2;
    *mr1 = a3;
    *mr2 = a4;
    *mr3 = a5;
    seL4_MessageInfo_t out;
    out.words[0] = a1;
    return out;
}

/* seL4_Recv (MCS): block on `ep` until a message arrives. Out-fills
 * *badge with the sender's badge (acts as our rcvid) and *mr0..*mr3
 * with the first four message words; longer messages spill into
 * ipcbuf->msg[4..]. Returns the message info tag.
 *
 * MCS adds the `reply` argument (register a6): the reply object the
 * kernel binds to the incoming Call so a later Send/ReplyRecv can
 * answer it.  Pass the caller's own reply object cap. */
static inline seL4_MessageInfo_t
qsoe_sys_recv(seL4_CPtr ep, seL4_Word *badge, seL4_CPtr reply,
              seL4_Word *mr0, seL4_Word *mr1, seL4_Word *mr2, seL4_Word *mr3)
{
    register seL4_Word a0 __asm__("a0") = ep;
    register seL4_Word a1 __asm__("a1");
    register seL4_Word a2 __asm__("a2");
    register seL4_Word a3 __asm__("a3");
    register seL4_Word a4 __asm__("a4");
    register seL4_Word a5 __asm__("a5");
    register seL4_Word a6 __asm__("a6") = reply;
    register seL4_Word a7 __asm__("a7") = (seL4_Word)SYS_Recv;
    __asm__ volatile("ecall"
                 : "+r"(a0), "=r"(a1), "=r"(a2), "=r"(a3), "=r"(a4), "=r"(a5)
                 : "r"(a6), "r"(a7)
                 : "memory");
    *badge = a0;
    *mr0 = a2; *mr1 = a3; *mr2 = a4; *mr3 = a5;
    seL4_MessageInfo_t out; out.words[0] = a1; return out;
}

/* MCS has no standalone seL4_Reply syscall: a reply is delivered by
 * Send-ing to the reply object cap the kernel bound at Recv time (see
 * qsoe_sys_send below — Send to a reply cap unblocks the original
 * caller).  ReplyRecv folds that Send into the next receive. */

/* seL4_ReplyRecv (MCS): atomic reply-to-previous + receive-next. The
 * taskman dispatch loop is built around this — it's the cheapest way to
 * process a stream of requests.  `reply` (register a6) is the reply
 * object the previous Recv bound to the caller being answered; the
 * kernel re-binds it to the next caller before returning. */
static inline seL4_MessageInfo_t
qsoe_sys_reply_recv(seL4_CPtr ep, seL4_MessageInfo_t info, seL4_Word *badge,
                    seL4_CPtr reply,
                    seL4_Word *mr0, seL4_Word *mr1, seL4_Word *mr2, seL4_Word *mr3)
{
    register seL4_Word a0 __asm__("a0") = ep;
    register seL4_Word a1 __asm__("a1") = info.words[0];
    register seL4_Word a2 __asm__("a2") = *mr0;
    register seL4_Word a3 __asm__("a3") = *mr1;
    register seL4_Word a4 __asm__("a4") = *mr2;
    register seL4_Word a5 __asm__("a5") = *mr3;
    register seL4_Word a6 __asm__("a6") = reply;
    register seL4_Word a7 __asm__("a7") = (seL4_Word)SYS_ReplyRecv;
    __asm__ volatile("ecall"
                 : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5)
                 : "r"(a6), "r"(a7)
                 : "memory");
    *badge = a0;
    *mr0 = a2; *mr1 = a3; *mr2 = a4; *mr3 = a5;
    seL4_MessageInfo_t out; out.words[0] = a1; return out;
}

/* seL4_Yield — drop the current thread to the back of its priority's
 * runqueue. On non-MCS, this lets equal-priority threads run; on a
 * busy system server in a spin loop, it prevents starving lower-prio
 * threads outright. (For our v0.3.2 dispatch loop, seL4_Recv blocks
 * the thread until a message arrives, making the explicit yield
 * unnecessary. But while taskman has no real work to do — e.g. in
 * v0.3.0's spawn-and-idle phase — yield is the right primitive.) */
static inline void qsoe_sys_yield(void)
{
    register seL4_Word a7 __asm__("a7") = (seL4_Word)SYS_Yield;
    __asm__ volatile("ecall" : : "r"(a7) : "memory");
}

/* seL4_Untyped_Retype: retype untyped into <num> objects of <type>, placed
 * starting at slot <node_offset> in CNode <root> (looked up via <node_index>,
 * <node_depth>). Returns the reply label (0 = success). */
static inline seL4_Word
qsoe_untyped_retype(seL4_CPtr untyped, seL4_Word type, seL4_Word size_bits,
                    seL4_CPtr root, seL4_Word node_index, seL4_Word node_depth,
                    seL4_Word node_offset, seL4_Word num_objects)
{
    qsoe_ipcbuf->caps_or_badges[0] = root;
    qsoe_ipcbuf->msg[4] = node_offset;
    qsoe_ipcbuf->msg[5] = num_objects;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_UntypedRetype, 0, 1, 6);
    seL4_Word mr0 = type, mr1 = size_bits, mr2 = node_index, mr3 = node_depth;
    seL4_MessageInfo_t reply = qsoe_sys_call(untyped, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_cnode_revoke(seL4_CPtr root, seL4_Word index, seL4_Uint8 depth)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_CNodeRevoke, 0, 0, 2);
    seL4_Word mr0 = index, mr1 = (seL4_Word)(depth & 0xffu), mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(root, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_cnode_delete(seL4_CPtr root, seL4_Word index, seL4_Uint8 depth)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_CNodeDelete, 0, 0, 2);
    seL4_Word mr0 = index, mr1 = (seL4_Word)(depth & 0xffu), mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(root, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_cnode_copy(seL4_CPtr dest_root, seL4_Word dest_index, seL4_Uint8 dest_depth,
                seL4_CPtr src_root, seL4_Word src_index, seL4_Uint8 src_depth,
                seL4_CapRights_t rights)
{
    qsoe_ipcbuf->caps_or_badges[0] = src_root;
    qsoe_ipcbuf->msg[4] = rights.words[0];

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_CNodeCopy, 0, 1, 5);
    seL4_Word mr0 = dest_index, mr1 = (seL4_Word)(dest_depth & 0xffu);
    seL4_Word mr2 = src_index,  mr3 = (seL4_Word)(src_depth  & 0xffu);
    seL4_MessageInfo_t reply = qsoe_sys_call(dest_root, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_cnode_mint(seL4_CPtr dest_root, seL4_Word dest_index, seL4_Uint8 dest_depth,
                seL4_CPtr src_root, seL4_Word src_index, seL4_Uint8 src_depth,
                seL4_CapRights_t rights, seL4_Word badge)
{
    qsoe_ipcbuf->caps_or_badges[0] = src_root;
    qsoe_ipcbuf->msg[4] = rights.words[0];
    qsoe_ipcbuf->msg[5] = badge;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_CNodeMint, 0, 1, 6);
    seL4_Word mr0 = dest_index, mr1 = (seL4_Word)(dest_depth & 0xffu);
    seL4_Word mr2 = src_index,  mr3 = (seL4_Word)(src_depth  & 0xffu);
    seL4_MessageInfo_t reply = qsoe_sys_call(dest_root, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* Minimal RISC-V user context for seL4_TCB_WriteRegisters. Matches the
 * order seL4 expects (see kernel/libsel4/arch_include/riscv/sel4/arch/types.h);
 * only the fields we actually set are commented. We never read the
 * trailing s/t registers — they're zero-initialised. */
typedef struct {
    seL4_Word pc;     /* set: entry point */
    seL4_Word ra;
    seL4_Word sp;     /* set: stack top */
    seL4_Word gp;     /* set: __global_pointer$ (or 0; tester's crt0 sets gp itself) */
    seL4_Word s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    seL4_Word a0;     /* set: first arg (we use it as pid) */
    seL4_Word a1, a2, a3, a4, a5, a6, a7;
    seL4_Word t0, t1, t2, t3, t4, t5, t6;
    seL4_Word tp;
} qsoe_user_ctx_t;

/* 32 register fields total in qsoe_user_ctx_t (pc, ra, sp, gp,
 * s0..s11, a0..a7, t0..t6, tp). Counts confirmed against the libsel4
 * WriteRegisters stub which fills msg[2..33] with 32 reg values. */
#define QSOE_USER_CTX_NREGS 32

static inline seL4_Word
qsoe_tcb_write_registers(seL4_CPtr tcb, int resume_target,
                         const qsoe_user_ctx_t *ctx)
{
    /* Message body: flags(1) + count(1) + 32 reg fields = 34 words.
     * mr0..mr3 hold flags, count, pc, ra. The remaining 30 reg fields
     * spill into ipcbuf->msg[4..33]. */
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_TCBWriteRegisters, 0, 0,
                                                   QSOE_USER_CTX_NREGS + 2);
    seL4_Word mr0 = (resume_target ? 1 : 0);
    seL4_Word mr1 = QSOE_USER_CTX_NREGS;
    seL4_Word mr2 = ctx->pc;
    seL4_Word mr3 = ctx->ra;
    const seL4_Word *src = &ctx->sp;          /* sp is index 2 in struct */
    for (unsigned i = 0; i < QSOE_USER_CTX_NREGS - 2; ++i) {
        qsoe_ipcbuf->msg[4 + i] = src[i];
    }
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* seL4_TCB_Configure (MCS): the fault endpoint moved out to
 * SetSchedParams/SetSpace, so Configure no longer carries it — 3 MRs
 * (cnode_data, vspace_data, ipc_buffer_vaddr) + 3 caps (cnode_root,
 * vspace_root, ipc_buffer_frame).  A TCB configured this way still
 * cannot run until a scheduling context is bound (see
 * qsoe_tcb_set_sched_params). */
static inline seL4_Word
qsoe_tcb_configure(seL4_CPtr tcb,
                   seL4_CPtr cnode_root, seL4_Word cnode_data,
                   seL4_CPtr vspace_root, seL4_Word vspace_data,
                   seL4_Word ipc_buffer_vaddr, seL4_CPtr ipc_buffer_frame)
{
    qsoe_ipcbuf->caps_or_badges[0] = cnode_root;
    qsoe_ipcbuf->caps_or_badges[1] = vspace_root;
    qsoe_ipcbuf->caps_or_badges[2] = ipc_buffer_frame;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_TCBConfigure, 0, 3, 3);
    seL4_Word mr0 = cnode_data;
    seL4_Word mr1 = vspace_data;
    seL4_Word mr2 = ipc_buffer_vaddr;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* seL4_TCB_SetSchedParams (MCS): set max-controlled-priority + priority,
 * AND bind a scheduling context + fault handler in one invocation —
 * 2 MRs (mcp, prio) + 3 caps (authority TCB, sched_context, fault EP).
 * Binding a (configured) SC is what makes the thread runnable.  Passing
 * fault_ep = 0 (a null cap) means "no fault handler".  Replaces the
 * non-MCS SetPriority + SetAffinity pair (affinity now comes from which
 * core's SchedControl configured the SC). */
static inline seL4_Word
qsoe_tcb_set_sched_params(seL4_CPtr tcb, seL4_CPtr authority,
                          seL4_Word mcp, seL4_Word prio,
                          seL4_CPtr sched_context, seL4_CPtr fault_ep)
{
    qsoe_ipcbuf->caps_or_badges[0] = authority;
    qsoe_ipcbuf->caps_or_badges[1] = sched_context;
    qsoe_ipcbuf->caps_or_badges[2] = fault_ep;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_TCBSetSchedParams, 0, 3, 2);
    seL4_Word mr0 = mcp, mr1 = prio, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* seL4_SchedControl_ConfigureFlags (MCS): program a scheduling context's
 * budget/period (microseconds) by invoking the per-core SchedControl cap
 * (bootinfo.schedcontrol.start + core).  budget == period yields a
 * round-robin / best-effort thread — the closest match to the non-MCS
 * scheduler QSOE/L ran before v0.10.  4 MRs + flags in msg[4], 1 cap
 * (the target SC). */
static inline seL4_Word
qsoe_sched_control_configure(seL4_CPtr sched_control, seL4_CPtr sc,
                             seL4_Word budget_us, seL4_Word period_us,
                             seL4_Word extra_refills, seL4_Word badge,
                             seL4_Word flags)
{
    qsoe_ipcbuf->caps_or_badges[0] = sc;
    qsoe_ipcbuf->msg[4] = flags;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_SchedControlConfigureFlags,
                                                  0, 1, 5);
    seL4_Word mr0 = budget_us, mr1 = period_us, mr2 = extra_refills, mr3 = badge;
    seL4_MessageInfo_t reply = qsoe_sys_call(sched_control, tag,
                                             &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_tcb_set_priority(seL4_CPtr tcb, seL4_CPtr authority_tcb, seL4_Word prio)
{
    qsoe_ipcbuf->caps_or_badges[0] = authority_tcb;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_TCBSetPriority, 0, 1, 1);
    seL4_Word mr0 = prio, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* Under MCS there is no TCB_SetAffinity: a thread runs on whichever
 * core's SchedControl cap configured its scheduling context.  Choose
 * the core by passing sched_control = bootinfo.schedcontrol.start +
 * core_index to qsoe_sched_control_configure.  v0.4's ThreadCreate
 * runmask hint now selects that SchedControl cap instead. */

static inline seL4_Word
qsoe_tcb_resume(seL4_CPtr tcb)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_TCBResume, 0, 0, 0);
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_tcb_suspend(seL4_CPtr tcb)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_TCBSuspend, 0, 0, 0);
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* seL4_TCB_BindNotification — attach a Notification to a TCB so that
 * signals on the notification wake the TCB even when blocked on a
 * regular endpoint Recv. The badge value carried on the wake is the
 * notification's accumulated notifyWord (OR of all Signal-cap badges).
 * On non-MCS each TCB binds at most one Notification; binding a second
 * fails until the first is unbound. v0.4.3 uses this for QNX-style
 * pulse delivery: each channel has a Notification, bound to the
 * owner's TCB, so a queued pulse wakes MsgReceive directly. */
static inline seL4_Word
qsoe_tcb_bind_notification(seL4_CPtr tcb, seL4_CPtr ntfn)
{
    qsoe_ipcbuf->caps_or_badges[0] = ntfn;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TCBBindNotification, 0, 1, 0);
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_tcb_unbind_notification(seL4_CPtr tcb)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TCBUnbindNotification, 0, 0, 0);
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* seL4_CNode_Move — move a cap from (src_root, src_index) to
 * (dest_root, dest_index), leaving the source slot empty.  Under MCS
 * this is how taskman stashes a deferred reply: move the dispatcher's
 * active reply-object cap out into a per-waiter slot (then replenish a
 * fresh reply object for the next Recv).  Later Send-ing to the stashed
 * slot delivers the deferred reply (see qsoe_sys_send).  Marshalling
 * mirrors CNodeCopy minus the rights word. */
static inline seL4_Word
qsoe_cnode_move(seL4_CPtr dest_root, seL4_Word dest_index, seL4_Uint8 dest_depth,
                seL4_CPtr src_root, seL4_Word src_index, seL4_Uint8 src_depth)
{
    qsoe_ipcbuf->caps_or_badges[0] = src_root;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_CNodeMove, 0, 1, 4);
    seL4_Word mr0 = dest_index, mr1 = (seL4_Word)(dest_depth & 0xffu);
    seL4_Word mr2 = src_index,  mr3 = (seL4_Word)(src_depth  & 0xffu);
    seL4_MessageInfo_t reply = qsoe_sys_call(dest_root, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* seL4_Send — one-shot Send on a cap. No reply, no implicit reply
 * state on the sender.  Two uses in QSOE/L MCS: (1) deliver a deferred
 * reply by Send-ing to a stashed reply-object cap, which unblocks the
 * original caller (the parent in waitpid(), a parked sleeper, ...);
 * (2) Signal a notification.  Unlike the non-MCS save-caller slot, an
 * MCS reply object is NOT self-cleared by the Send — the caller must
 * delete/recycle it (see tm_reply_deliver). */
static inline void
qsoe_sys_send(seL4_CPtr ep, seL4_MessageInfo_t info,
              seL4_Word mr0, seL4_Word mr1, seL4_Word mr2, seL4_Word mr3)
{
    register seL4_Word a0 __asm__("a0") = ep;
    register seL4_Word a1 __asm__("a1") = info.words[0];
    register seL4_Word a2 __asm__("a2") = mr0;
    register seL4_Word a3 __asm__("a3") = mr1;
    register seL4_Word a4 __asm__("a4") = mr2;
    register seL4_Word a5 __asm__("a5") = mr3;
    register seL4_Word a7 __asm__("a7") = (seL4_Word)SYS_Send;
    __asm__ volatile("ecall"
                 : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5)
                 : "r"(a7)
                 : "memory");
}

/* seL4_Signal — empty Send to a Notification cap (kernel checks cap
 * type). No reply, no MRs. */
static inline void
qsoe_sys_signal(seL4_CPtr ntfn)
{
    register seL4_Word a0 __asm__("a0") = ntfn;
    register seL4_Word a1 __asm__("a1") = 0;  /* tag: label=0, length=0 */
    register seL4_Word a7 __asm__("a7") = (seL4_Word)SYS_Send;
    __asm__ volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a7)
                 : "memory");
}

/* v0.6.1 IRQ wrappers.
 *
 * QSOE drivers attach to a PLIC interrupt line by minting an
 * IRQHandler cap from the rootserver's IRQControl, then binding a
 * Notification to it. The kernel signals the Notification whenever
 * the IRQ fires; the driver acks via the IRQHandler to re-arm.
 *
 * On RISC-V, mint goes through RISCVIRQIssueIRQHandlerTrigger
 * (the architecture-specific variant — the generic IRQIssueIRQHandler
 * isn't exposed). trigger=0 means level-triggered (correct for the
 * 16550 UART on QEMU virt's PLIC). */

static inline seL4_Word
qsoe_irq_control_get(seL4_CPtr ctrl, seL4_Word irq, seL4_Word trigger,
                     seL4_CPtr root, seL4_Word index, seL4_Uint8 depth)
{
    qsoe_ipcbuf->caps_or_badges[0] = root;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(RISCVIRQIssueIRQHandlerTrigger,
                                                   0, 1, 4);
    seL4_Word mr0 = irq;
    seL4_Word mr1 = trigger;
    seL4_Word mr2 = index;
    seL4_Word mr3 = (seL4_Word)(depth & 0xffu);
    seL4_MessageInfo_t reply = qsoe_sys_call(ctrl, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_irq_handler_set_notification(seL4_CPtr handler, seL4_CPtr ntfn)
{
    qsoe_ipcbuf->caps_or_badges[0] = ntfn;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(IRQSetIRQHandler, 0, 1, 0);
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(handler, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_irq_handler_ack(seL4_CPtr handler)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(IRQAckIRQ, 0, 0, 0);
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(handler, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/* seL4_Wait (MCS): block on a Notification cap.  MCS gives Wait its own
 * syscall number (distinct from Recv) and, unlike Recv, it takes no
 * reply object — you can't reply to a notification.  Returns the badge
 * (or 0 if the Notification cap is unbadged). */
static inline seL4_Word
qsoe_sys_wait(seL4_CPtr ntfn)
{
    register seL4_Word a0 __asm__("a0") = ntfn;
    register seL4_Word a1 __asm__("a1");
    register seL4_Word a2 __asm__("a2");
    register seL4_Word a3 __asm__("a3");
    register seL4_Word a4 __asm__("a4");
    register seL4_Word a5 __asm__("a5");
    register seL4_Word a7 __asm__("a7") = (seL4_Word)SYS_Wait;
    __asm__ volatile("ecall"
                 : "+r"(a0), "=r"(a1), "=r"(a2), "=r"(a3), "=r"(a4), "=r"(a5)
                 : "r"(a7)
                 : "memory");
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return a0;
}

/* RISC-V VM attributes. Bit 0 = ExecuteNever; we leave it 0 for code. */
#define QSOE_VM_ATTR_DEFAULT 0

static inline seL4_Word
qsoe_riscv_pagetable_map(seL4_CPtr pt, seL4_CPtr vspace,
                         seL4_Word vaddr, seL4_Word attr)
{
    qsoe_ipcbuf->caps_or_badges[0] = vspace;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_RISCVPageTableMap, 0, 1, 2);
    seL4_Word mr0 = vaddr, mr1 = attr, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(pt, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_riscv_page_map(seL4_CPtr page, seL4_CPtr vspace,
                    seL4_Word vaddr, seL4_CapRights_t rights, seL4_Word attr)
{
    qsoe_ipcbuf->caps_or_badges[0] = vspace;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_RISCVPageMap, 0, 1, 3);
    seL4_Word mr0 = vaddr, mr1 = rights.words[0], mr2 = attr, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(page, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_riscv_page_unmap(seL4_CPtr page)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_RISCVPageUnmap, 0, 0, 0);
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(page, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

static inline seL4_Word
qsoe_riscv_asidpool_assign(seL4_CPtr asid_pool, seL4_CPtr vspace)
{
    qsoe_ipcbuf->caps_or_badges[0] = vspace;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_RISCVASIDPoolAssign, 0, 1, 0);
    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(asid_pool, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

/*
 * LQ-private continuation of retired wire opcode 0x114.  The shared
 * <qsoe/wire.h> dropped TM_REQ_DUP_CAP in favor of the
 * ConnectServerInfo + ConnectAttach(index_hint) + _IO_DUP idiom (the
 * shape NQ implements); LQ's dup2 / fcntl(F_DUPFD) seam and the
 * taskman dispatcher still ride the cap-copy form.  The value stays
 * 0x114 -- the slot is documented as reserved in wire.h, so nothing
 * else can claim it.  Delete together with the migration.
 */
#define TM_REQ_DUP_CAP  0x114

#endif /* QSOE_INVOKE_H */
