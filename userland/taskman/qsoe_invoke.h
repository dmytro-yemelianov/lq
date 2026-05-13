/*
 * qsoe_invoke.h — hand-rolled seL4 capability invocations for taskman.
 *
 * Mirrors the marshalling done by libsel4's generated stubs (verified against
 * `python3 syscall_stub_gen.py -a riscv64 ...` output, methods we use:
 *   UntypedRetype, CNodeRevoke, CNodeDelete, CNodeCopy, CNodeMint).
 *
 * The IPC buffer pointer must be initialised once via qsoe_invoke_init()
 * before any call below. For taskman (single-threaded in v0.x) we keep
 * it as a plain global; when threads land we'll move to TLS.
 */
#ifndef QSOE_INVOKE_H
#define QSOE_INVOKE_H

#include "sel4_types.h"

extern seL4_IPCBuffer *qsoe_ipcbuf;

static inline void qsoe_invoke_init(seL4_IPCBuffer *buf)
{
    qsoe_ipcbuf = buf;
}

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

#endif /* QSOE_INVOKE_H */
