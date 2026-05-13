/*
 * qsoe_invoke.h — hand-rolled seL4 capability invocations for taskman.
 *
 * Mirrors the marshalling done by libsel4's generated stubs (verified against
 * `python3 syscall_stub_gen.py -a riscv64 ...` output, methods we use:
 *   UntypedRetype, CNodeRevoke, CNodeDelete, CNodeCopy, CNodeMint).
 *
 * The IPC buffer pointer lives in the current thread's qsoe_tcb_t
 * (see <qsoe/tls.h>) — the crt0 plants &qsoe_main_tcb in tp before
 * any call below, and qsoe_libqsoe_init() then writes the buffer
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
    register seL4_Word a0 asm("a0") = dest;
    register seL4_Word a1 asm("a1") = info.words[0];
    register seL4_Word a2 asm("a2") = *mr0;
    register seL4_Word a3 asm("a3") = *mr1;
    register seL4_Word a4 asm("a4") = *mr2;
    register seL4_Word a5 asm("a5") = *mr3;
    register seL4_Word a7 asm("a7") = (seL4_Word)SYS_Call;
    asm volatile("ecall"
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

/* seL4_Recv: block on `ep` until a message arrives. Out-fills *badge
 * with the sender's badge (acts as our rcvid) and *mr0..*mr3 with the
 * first four message words; longer messages spill into ipcbuf->msg[4..].
 * Returns the message info tag. */
static inline seL4_MessageInfo_t
qsoe_sys_recv(seL4_CPtr ep, seL4_Word *badge,
              seL4_Word *mr0, seL4_Word *mr1, seL4_Word *mr2, seL4_Word *mr3)
{
    register seL4_Word a0 asm("a0") = ep;
    register seL4_Word a1 asm("a1");
    register seL4_Word a2 asm("a2");
    register seL4_Word a3 asm("a3");
    register seL4_Word a4 asm("a4");
    register seL4_Word a5 asm("a5");
    register seL4_Word a7 asm("a7") = (seL4_Word)SYS_Recv;
    asm volatile("ecall"
                 : "+r"(a0), "=r"(a1), "=r"(a2), "=r"(a3), "=r"(a4), "=r"(a5)
                 : "r"(a7)
                 : "memory");
    *badge = a0;
    *mr0 = a2; *mr1 = a3; *mr2 = a4; *mr3 = a5;
    seL4_MessageInfo_t out; out.words[0] = a1; return out;
}

/* seL4_Reply: reply to the previous Recv's implicit caller cap (the
 * one the kernel stashed in tcbCaller on Recv). No destination param —
 * non-MCS reply is per-thread, not per-cap. */
static inline void
qsoe_sys_reply(seL4_MessageInfo_t info,
               seL4_Word mr0, seL4_Word mr1, seL4_Word mr2, seL4_Word mr3)
{
    register seL4_Word a1 asm("a1") = info.words[0];
    register seL4_Word a2 asm("a2") = mr0;
    register seL4_Word a3 asm("a3") = mr1;
    register seL4_Word a4 asm("a4") = mr2;
    register seL4_Word a5 asm("a5") = mr3;
    register seL4_Word a7 asm("a7") = (seL4_Word)SYS_Reply;
    asm volatile("ecall"
                 : "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5)
                 : "r"(a7)
                 : "memory");
}

/* seL4_ReplyRecv: atomic reply-to-previous + receive-next. The taskman
 * dispatch loop is built around this — it's the cheapest way to process
 * a stream of requests. */
static inline seL4_MessageInfo_t
qsoe_sys_reply_recv(seL4_CPtr ep, seL4_MessageInfo_t info, seL4_Word *badge,
                    seL4_Word *mr0, seL4_Word *mr1, seL4_Word *mr2, seL4_Word *mr3)
{
    register seL4_Word a0 asm("a0") = ep;
    register seL4_Word a1 asm("a1") = info.words[0];
    register seL4_Word a2 asm("a2") = *mr0;
    register seL4_Word a3 asm("a3") = *mr1;
    register seL4_Word a4 asm("a4") = *mr2;
    register seL4_Word a5 asm("a5") = *mr3;
    register seL4_Word a7 asm("a7") = (seL4_Word)SYS_ReplyRecv;
    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3), "+r"(a4), "+r"(a5)
                 : "r"(a7)
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
    register seL4_Word a7 asm("a7") = (seL4_Word)SYS_Yield;
    asm volatile("ecall" : : "r"(a7) : "memory");
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

static inline seL4_Word
qsoe_tcb_configure(seL4_CPtr tcb, seL4_CPtr fault_ep,
                   seL4_CPtr cnode_root, seL4_Word cnode_data,
                   seL4_CPtr vspace_root, seL4_Word vspace_data,
                   seL4_Word ipc_buffer_vaddr, seL4_CPtr ipc_buffer_frame)
{
    qsoe_ipcbuf->caps_or_badges[0] = cnode_root;
    qsoe_ipcbuf->caps_or_badges[1] = vspace_root;
    qsoe_ipcbuf->caps_or_badges[2] = ipc_buffer_frame;

    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_TCBConfigure, 0, 3, 4);
    seL4_Word mr0 = fault_ep;
    seL4_Word mr1 = cnode_data;
    seL4_Word mr2 = vspace_data;
    seL4_Word mr3 = ipc_buffer_vaddr;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
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

/* seL4_TCB_SetAffinity (SMP only). Pins the given TCB to logical CPU
 * `affinity` (0..CONFIG_MAX_NUM_NODES-1). New TCBs default to CPU 0;
 * call this right after Configure (and before Resume) to spread work
 * across harts. v0.4's ThreadCreate uses _thread_attr.runmask as the
 * affinity hint. */
static inline seL4_Word
qsoe_tcb_set_affinity(seL4_CPtr tcb, seL4_Word affinity)
{
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(INV_TCBSetAffinity, 0, 0, 1);
    seL4_Word mr0 = affinity, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t reply = qsoe_sys_call(tcb, tag, &mr0, &mr1, &mr2, &mr3);
    return seL4_MessageInfo_get_label(reply);
}

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

/* seL4_Signal — empty Send to a Notification cap (kernel checks cap
 * type). No reply, no MRs. */
static inline void
qsoe_sys_signal(seL4_CPtr ntfn)
{
    register seL4_Word a0 asm("a0") = ntfn;
    register seL4_Word a1 asm("a1") = 0;  /* tag: label=0, length=0 */
    register seL4_Word a7 asm("a7") = (seL4_Word)SYS_Send;
    asm volatile("ecall"
                 : "+r"(a0), "+r"(a1)
                 : "r"(a7)
                 : "memory");
}

/* seL4_Wait — same syscall as Recv; kernel distinguishes by cap type.
 * Returns the badge (or 0 if the Notification cap is unbadged). */
static inline seL4_Word
qsoe_sys_wait(seL4_CPtr ntfn)
{
    register seL4_Word a0 asm("a0") = ntfn;
    register seL4_Word a1 asm("a1");
    register seL4_Word a2 asm("a2");
    register seL4_Word a3 asm("a3");
    register seL4_Word a4 asm("a4");
    register seL4_Word a5 asm("a5");
    register seL4_Word a7 asm("a7") = (seL4_Word)SYS_Recv;
    asm volatile("ecall"
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

#endif /* QSOE_INVOKE_H */
