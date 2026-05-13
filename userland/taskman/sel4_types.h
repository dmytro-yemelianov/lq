/*
 * sel4_types.h - minimal seL4 type and structure definitions for taskman.
 *
 * These mirror the layouts that libsel4 produces on RISC-V 64-bit / non-MCS.
 * We define them ourselves rather than including libsel4 because v0.x does
 * not yet pull libsel4 into the QSOE build; switching is planned for v0.3.
 *
 * Cross-check, if a future kernel update changes ABIs:
 *   sel4test-full/kernel/libsel4/include/sel4/bootinfo_types.h
 *   sel4test-full/kernel/libsel4/include/sel4/types.h
 *   sel4test-full/build-qsoe-riscv64/kernel/gen_headers/api/invocation.h
 */
#ifndef QSOE_SEL4_TYPES_H
#define QSOE_SEL4_TYPES_H

typedef unsigned long       seL4_Word;
typedef seL4_Word           seL4_CPtr;
typedef seL4_Word           seL4_NodeId;
typedef unsigned char       seL4_Uint8;
typedef unsigned int        seL4_Uint32;
typedef unsigned long       seL4_Domain;

/* Bitfield-packed MessageInfo — packed as one 64-bit word. */
typedef struct { seL4_Word words[1]; } seL4_MessageInfo_t;

typedef struct { seL4_Word words[1]; } seL4_CapRights_t;

/* Object types (non-MCS, RISC-V 64; see objecttype.h enums). */
#define seL4_UntypedObject           0
#define seL4_TCBObject               1
#define seL4_EndpointObject          2
#define seL4_NotificationObject      3
#define seL4_CapTableObject          4
#define seL4_RISCV_Giga_Page         5
#define seL4_RISCV_4K_Page           6
#define seL4_RISCV_Mega_Page         7
#define seL4_RISCV_PageTableObject   8

/* Object size bits — for retype size_bits argument. */
#define seL4_TCBBits             10  /* TCB is 2^10 = 1024 bytes (non-MCS) */
#define seL4_EndpointBits         4
#define seL4_NotificationBits     5  /* notification_t is 2^5 = 32 bytes (non-MCS) */
#define seL4_PageBits            12  /* 4 KiB */
#define seL4_PageTableBits       12  /* one PT level on Sv39 */
#define seL4_VSpaceBits          seL4_PageTableBits

/* Initial CSpace fixed slots (see bootinfo.h). */
#define seL4_CapNull                  0
#define seL4_CapInitThreadTCB         1
#define seL4_CapInitThreadCNode       2
#define seL4_CapInitThreadVSpace      3
#define seL4_CapIRQControl            4
#define seL4_CapASIDControl           5
#define seL4_CapInitThreadASIDPool    6
#define seL4_CapIOPortControl         7
#define seL4_CapIOSpace               8
#define seL4_CapBootInfoFrame         9
#define seL4_CapInitThreadIPCBuffer  10

/* Invocation method labels, derived by counting the enum in
 * sel4test-full/build-qsoe-riscv64/kernel/gen_headers/api/invocation.h
 * with our config (non-MCS, SMP=ON with NUM_NODES=4, non-HW_DEBUG_API).
 *
 * v0.3.4: SMP inserts TCBSetAffinity at position 15, shifting every
 * later label by +1. nInvocationLabels = 34 (was 33), so arch-
 * specific labels start at 34. */
#define INV_UntypedRetype            1
#define INV_TCBWriteRegisters        3
#define INV_TCBConfigure             5   /* non-MCS variant */
#define INV_TCBSetPriority           6
#define INV_TCBSuspend              11
#define INV_TCBResume               12
#define INV_TCBSetAffinity          15   /* SMP-only — v0.4 uses */
#define INV_CNodeRevoke             18
#define INV_CNodeDelete             19
#define INV_CNodeCopy               21
#define INV_CNodeMint               22
/* Arch-specific labels start at nInvocationLabels (34 with SMP). */
#define INV_RISCVPageTableMap       34
#define INV_RISCVPageTableUnmap     35
#define INV_RISCVPageMap            36
#define INV_RISCVPageUnmap          37
#define INV_RISCVASIDPoolAssign     40

/* Fast-path syscall numbers (negative; see arch/api/syscall.h). */
#define SYS_Call                (-1)
#define SYS_ReplyRecv           (-2)
#define SYS_Send                (-3)
#define SYS_NBSend              (-4)
#define SYS_Recv                (-5)
#define SYS_Reply               (-6)
#define SYS_Yield               (-7)
#define SYS_NBRecv              (-8)

#define seL4_MsgMaxLength       120
#define seL4_MsgMaxExtraCaps    3

typedef struct seL4_IPCBuffer {
    seL4_MessageInfo_t tag;
    seL4_Word msg[seL4_MsgMaxLength];
    seL4_Word userData;
    seL4_Word caps_or_badges[seL4_MsgMaxExtraCaps];
    seL4_CPtr receiveCNode;
    seL4_CPtr receiveIndex;
    seL4_Word receiveDepth;
} seL4_IPCBuffer;

typedef struct {
    seL4_Word start;
    seL4_Word end;
} seL4_SlotRegion;

typedef struct {
    seL4_Word  paddr;
    seL4_Uint8 sizeBits;
    seL4_Uint8 isDevice;
    seL4_Uint8 padding[sizeof(seL4_Word) - 2 * sizeof(seL4_Uint8)];
} seL4_UntypedDesc;

typedef struct {
    seL4_Word        extraLen;
    seL4_NodeId      nodeID;
    seL4_Word        numNodes;
    seL4_Word        numIOPTLevels;
    seL4_IPCBuffer  *ipcBuffer;
    seL4_SlotRegion  empty;
    seL4_SlotRegion  sharedFrames;
    seL4_SlotRegion  userImageFrames;
    seL4_SlotRegion  userImagePaging;
    seL4_SlotRegion  ioSpaceCaps;
    seL4_SlotRegion  extraBIPages;
    seL4_Word        initThreadCNodeSizeBits;
    seL4_Domain      initThreadDomain;
    seL4_SlotRegion  untyped;
    seL4_UntypedDesc untypedList[];
} seL4_BootInfo;

/* Constructs a MessageInfo word from its components.
 * Layout (per RISC-V 64, non-MCS):
 *   bits  0.. 6 : length (7 bits)
 *   bits  7.. 8 : extraCaps (2)
 *   bits  9..11 : capsUnwrapped (3)
 *   bits 12..63 : label (52)
 */
static inline seL4_MessageInfo_t
seL4_MessageInfo_new(seL4_Word label, seL4_Word capsUnwrapped,
                     seL4_Word extraCaps, seL4_Word length)
{
    seL4_MessageInfo_t info;
    info.words[0] = (length & 0x7f)
                  | ((extraCaps & 0x3)  << 7)
                  | ((capsUnwrapped & 0x7) << 9)
                  | (label << 12);
    return info;
}

static inline seL4_Word
seL4_MessageInfo_get_label(seL4_MessageInfo_t info)
{
    return info.words[0] >> 12;
}

static inline seL4_Word
seL4_MessageInfo_get_length(seL4_MessageInfo_t info)
{
    return info.words[0] & 0x7f;
}

/* CapRights bit layout (RISC-V 64, from shared_types.bf):
 *   bit 0: capAllowWrite       — sender can transmit (Send)
 *   bit 1: capAllowRead        — receiver can receive (Recv)
 *   bit 2: capAllowGrant       — can transmit capabilities in messages
 *   bit 3: capAllowGrantReply  — can transmit a reply capability
 */
static inline seL4_CapRights_t
seL4_CapRights_new(seL4_Word grantReply, seL4_Word grant,
                   seL4_Word read, seL4_Word write)
{
    seL4_CapRights_t r;
    r.words[0] = (write      ? 1 : 0)
               | (read       ? 2 : 0)
               | (grant      ? 4 : 0)
               | (grantReply ? 8 : 0);
    return r;
}

#define QSOE_RIGHTS_ALL    seL4_CapRights_new(1, 1, 1, 1)
#define QSOE_RIGHTS_SEND   seL4_CapRights_new(1, 0, 0, 1) /* write + grantReply */

#endif /* QSOE_SEL4_TYPES_H */
