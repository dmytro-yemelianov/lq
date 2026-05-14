/*
 * spawn.c — minimal user-space process spawner for QSOE.
 *
 * Hand-rolled per plan §3 — no libsel4utils. Operates on a single
 * static "spawn context" since v0.3 only ever spawns one process at a
 * time. The full process-table machinery lives in server.[ch] and gets
 * populated here at the end of a successful spawn.
 */

#include "spawn.h"
#include "sel4_syscalls.h"
#include "qsoe_invoke.h"
#include "server.h"
#include "../libqsoe/include/qsoe/slots.h"

/* ELF64 minimal types — just enough to walk PHDRs. */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;

struct elf64_hdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct elf64_phdr {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
};

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2
#define PF_R    4

/* Scratch vaddr in taskman's VSpace, used to memcpy ELF bytes into a
 * frame before we Page_Map that frame into the child's VSpace. The
 * value is chosen so the intermediate page tables that the kernel
 * already set up to cover taskman's user image also cover it
 * (everything in the same Sv39 2 MiB region). */
#define TM_SCRATCH_VADDR 0x100000UL

/* Child VSpace layout. Everything fits in one Sv39 2 MiB region so a
 * single L1 PT + single L0 PT covers it. The stack lives in two pages
 * directly below the IPC buffer; sp starts at CHILD_STACK_TOP and grows
 * down through the two pages [CHILD_STACK_BASE, CHILD_STACK_TOP). The
 * top page also holds the SysV ABI initial-stack image (argc/argv/
 * envp/auxv/strings) that the crt0 in start.S reads at entry. */
#define CHILD_IMAGE_BASE   0x10000UL   /* matches tester's linker script */
#define CHILD_STACK_BASE   0x1FC000UL  /* 2 stack pages: [0x1FC000, 0x1FE000) */
#define CHILD_STACK_TOP    0x1FE000UL  /* sp starts here, grows down */
#define CHILD_STACK_PAGES  2
#define CHILD_IPC_BUFFER   0x1FE000UL  /* one page, just above the stack */

/* Image can grow up to one Sv39 L0 PT's coverage — 2 MiB. Beyond that
 * we'd need ensure_l0_pt() to lazily allocate per-2-MiB-region PTs as
 * the loader walks pages; deferred to whenever an image actually
 * exceeds 2 MiB. With the worker region now at 0x40000000 (a separate
 * L1 PT, see [[project-image-size-cap]]), images and workers never
 * collide. */
#define IMAGE_MAX_BYTES    (2UL * 1024 * 1024 - CHILD_IMAGE_BASE)

/* zero-and-memcpy helpers — we run with -fno-builtin and no libc. */
static void qmemcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
}

static void qmemset(void *dst, int v, unsigned long n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)v;
}

/* Map a frame temporarily into taskman's vspace at TM_SCRATCH_VADDR. */
static int scratch_map(seL4_CPtr frame)
{
    return (int)qsoe_riscv_page_map(frame, seL4_CapInitThreadVSpace,
                                    TM_SCRATCH_VADDR,
                                    QSOE_RIGHTS_ALL,
                                    QSOE_VM_ATTR_DEFAULT);
}

static int scratch_unmap(seL4_CPtr frame)
{
    return (int)qsoe_riscv_page_unmap(frame);
}

/* The shared spawn state — single-shot for v0.3.0. */
extern seL4_CPtr s_untyped;   /* defined in server.c */
extern seL4_CPtr s_cnode_root;
extern seL4_CPtr s_next_slot;

/* Allocate one untyped retype into the next free slot. Returns the
 * slot on success, 0 on failure. */
static seL4_CPtr alloc_object(seL4_Word type, seL4_Word size_bits)
{
    seL4_CPtr slot = s_next_slot++;
    seL4_Word err = qsoe_untyped_retype(s_untyped, type, size_bits,
                                         s_cnode_root, 0, 0, slot, 1);
    return (err == 0) ? slot : 0;
}

static unsigned long qstrlen(const char *s)
{
    unsigned long n = 0;
    while (s[n]) ++n;
    return n;
}

/* Build the SysV ABI initial-stack image into the top page of the
 * child's stack region.
 *
 * Layout from high to low (the child sees sp pointing at argc):
 *   [string area: argv[0]\0 argv[1]\0 ... envp[0]\0 envp[1]\0 ...]
 *   [auxv terminator: a_type = AT_NULL = 0, a_un = 0]   16 bytes
 *   [envp NULL terminator]                               8 bytes
 *   [envp[envc-1]]                                       8 bytes
 *   ...
 *   [envp[0]]                                            8 bytes
 *   [argv NULL terminator]                               8 bytes
 *   [argv[argc-1]]
 *   ...
 *   [argv[0]]
 *   [argc]                                               8 bytes  <-- sp
 *
 * sp is held 16-byte aligned per RISC-V SysV by padding the string
 * area upward as needed.
 *
 * The page is mapped temporarily into taskman's vspace at the scratch
 * vaddr so we can write into it before mapping it into the child.
 *
 * Returns the child-vspace address of argc (= initial sp). 0 if the
 * combined size exceeds one stack page (caller may grow then). */
static unsigned long build_initial_stack(seL4_CPtr top_frame,
                                          int argc, const char *const *argv,
                                          int envc, const char *const *envp)
{
    /* Compute total bytes needed for the string area. */
    unsigned long strs_bytes = 0;
    for (int i = 0; i < argc; ++i) strs_bytes += qstrlen(argv[i]) + 1;
    for (int i = 0; i < envc; ++i) strs_bytes += qstrlen(envp[i]) + 1;

    /* Pointer + terminator area below the strings. */
    unsigned long below = 8 /*argc*/
                        + 8UL * (unsigned long)(argc + 1) /*argv + NULL*/
                        + 8UL * (unsigned long)(envc + 1) /*envp + NULL*/
                        + 16 /*auxv AT_NULL pair*/;

    /* Pad the string area so total is 16-aligned (initial sp 16-aligned). */
    unsigned long total = strs_bytes + below;
    unsigned long total_aligned = (total + 15UL) & ~15UL;
    unsigned long strs_alloc = total_aligned - below;

    if (total_aligned > 0x1000UL) return 0;  /* doesn't fit in one page */

    /* Map the frame into taskman's vspace, then write top-down. */
    if (scratch_map(top_frame) != 0) return 0;
    qmemset((void *)TM_SCRATCH_VADDR, 0, 0x1000);

    /* "Top" of the page in taskman's view; the child sees this same
     * byte at CHILD_STACK_TOP. */
    unsigned char *scratch_top = (unsigned char *)TM_SCRATCH_VADDR + 0x1000;
    unsigned long  child_top   = CHILD_STACK_TOP;

    /* String area sits at the very top, occupying strs_alloc bytes. */
    unsigned char *strs_scratch  = scratch_top - strs_alloc;
    unsigned long  strs_in_child = child_top   - strs_alloc;

    /* Per-string pointers we'll write into the argv/envp arrays. */
    unsigned long child_argv[16];
    unsigned long child_envp[16];
    /* (16 is enough for v0.4.4 demos; static array keeps stack frame
     * small. Larger arg lists would overflow the IPC-buffer payload
     * anyway.) */

    unsigned char *cur = strs_scratch;
    unsigned long  cur_child = strs_in_child;
    for (int i = 0; i < argc; ++i) {
        unsigned long len = qstrlen(argv[i]) + 1;
        qmemcpy(cur, argv[i], len);
        child_argv[i] = cur_child;
        cur += len;
        cur_child += len;
    }
    for (int i = 0; i < envc; ++i) {
        unsigned long len = qstrlen(envp[i]) + 1;
        qmemcpy(cur, envp[i], len);
        child_envp[i] = cur_child;
        cur += len;
        cur_child += len;
    }

    /* Now write the auxv/envp/argv arrays + argc below the strings.
     * `p` walks downward in 8-byte units. */
    unsigned long *p = (unsigned long *)strs_scratch;
    *--p = 0;                                 /* auxv: a_un */
    *--p = 0;                                 /* auxv: a_type = AT_NULL */
    *--p = 0;                                 /* envp NULL */
    for (int i = envc - 1; i >= 0; --i) *--p = child_envp[i];
    *--p = 0;                                 /* argv NULL */
    for (int i = argc - 1; i >= 0; --i) *--p = child_argv[i];
    *--p = (unsigned long)argc;               /* argc — sp points here */

    /* Final sp in child = child_top - total_aligned. */
    unsigned long sp_in_child = child_top - total_aligned;

    /* fence then unmap. */
    __asm__ volatile ("fence rw, rw" ::: "memory");
    if (scratch_unmap(top_frame) != 0) return 0;
    return sp_in_child;
}

int tm_spawn(const void *elf_blob, unsigned long elf_len,
             pid_t pid, seL4_CPtr primary_ep,
             int argc, const char *const *argv,
             int envc, const char *const *envp)
{
    (void)elf_len;
    const struct elf64_hdr *eh = elf_blob;

    /* Sanity-check the ELF header. */
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        sel4_debug_puts("spawn: not an ELF\n");
        return -EINVAL;
    }
    if (eh->e_ident[4] != 2 /* ELFCLASS64 */) {
        sel4_debug_puts("spawn: not ELF64\n");
        return -EINVAL;
    }

    /* 1. Allocate the child's kernel objects.
     *
     *    Order matters for our bump allocator (s_next_slot) — every
     *    failure path leaks slots until v0.4's PCB-keyed cleanup, but
     *    on success-path it's tight. */
    seL4_CPtr cnode = alloc_object(seL4_CapTableObject, 12);
    if (!cnode) return -ENOMEM;
    seL4_CPtr vspace = alloc_object(seL4_RISCV_PageTableObject, 0);
    if (!vspace) return -ENOMEM;
    seL4_CPtr tcb    = alloc_object(seL4_TCBObject, 0);
    if (!tcb) return -ENOMEM;

    /* Assign the new VSpace to taskman's ASID pool — required before
     * any Page_Map can succeed on it. */
    seL4_Word err = qsoe_riscv_asidpool_assign(seL4_CapInitThreadASIDPool, vspace);
    if (err) {
        sel4_debug_puts("spawn: ASIDPool_Assign failed\n");
        return -ENOMEM;
    }

    /* 2. Build the child's page-table tree. We need an L1 PT
     *    (covering [0, 1 GiB)) and an L0 PT (covering [0, 2 MiB)).
     *    Sv39 with 4 KiB pages → call PageTable_Map at each
     *    intermediate level. The kernel decides the level from vaddr. */
    seL4_CPtr l1_pt = alloc_object(seL4_RISCV_PageTableObject, 0);
    if (!l1_pt) return -ENOMEM;
    err = qsoe_riscv_pagetable_map(l1_pt, vspace, 0, QSOE_VM_ATTR_DEFAULT);
    if (err) { sel4_debug_puts("spawn: L1 PageTable_Map failed\n"); return -ENOMEM; }

    seL4_CPtr l0_pt = alloc_object(seL4_RISCV_PageTableObject, 0);
    if (!l0_pt) return -ENOMEM;
    err = qsoe_riscv_pagetable_map(l0_pt, vspace, 0, QSOE_VM_ATTR_DEFAULT);
    if (err) { sel4_debug_puts("spawn: L0 PageTable_Map failed\n"); return -ENOMEM; }

    /* 3. Walk PT_LOAD segments. For each page in the segment:
     *    allocate frame, scratch-map into taskman, memcpy the ELF
     *    bytes (zero-fill the BSS tail), scratch-unmap, then
     *    Page_Map into the child's VSpace. */
    const struct elf64_phdr *ph = (const struct elf64_phdr *)
                                  ((const u8 *)elf_blob + eh->e_phoff);
    for (u16 i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;

        u64 vstart = ph[i].p_vaddr & ~0xFFFUL;
        u64 vend   = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFFUL) & ~0xFFFUL;
        u64 filesz = ph[i].p_filesz;
        const u8 *src = (const u8 *)elf_blob + ph[i].p_offset;

        for (u64 v = vstart; v < vend; v += 0x1000) {
            seL4_CPtr frame = alloc_object(seL4_RISCV_4K_Page, 0);
            if (!frame) return -ENOMEM;

            err = scratch_map(frame);
            if (err) { sel4_debug_puts("spawn: scratch_map failed\n"); return -ENOMEM; }

            /* Compute how many bytes of FileSiz fall in this page. */
            u64 page_off = v - vstart;
            u64 in_page_start = (v < ph[i].p_vaddr) ?
                                (ph[i].p_vaddr - v) : 0;
            u64 file_off_at_v = (v > ph[i].p_vaddr) ?
                                (v - ph[i].p_vaddr) : 0;
            u64 file_bytes_this_page = 0;
            if (file_off_at_v < filesz) {
                file_bytes_this_page = filesz - file_off_at_v;
                if (file_bytes_this_page > 0x1000 - in_page_start) {
                    file_bytes_this_page = 0x1000 - in_page_start;
                }
            }
            (void)page_off;

            qmemset((void *)TM_SCRATCH_VADDR, 0, 0x1000);
            if (file_bytes_this_page) {
                qmemcpy((u8 *)TM_SCRATCH_VADDR + in_page_start,
                        src + file_off_at_v,
                        file_bytes_this_page);
            }

            /* fence.i so the new mapping observes our writes before
             * the child tries to execute them. The kernel issues a
             * fence on the next mapping op anyway, but be explicit. */
            __asm__ volatile ("fence rw, rw" ::: "memory");

            err = scratch_unmap(frame);
            if (err) { sel4_debug_puts("spawn: scratch_unmap failed\n"); return -ENOMEM; }

            /* Map into the child. Permissions follow the PHDR flags. */
            seL4_CapRights_t rights = seL4_CapRights_new(
                0,
                0,
                (ph[i].p_flags & PF_R) ? 1 : 0,
                (ph[i].p_flags & PF_W) ? 1 : 0);
            err = qsoe_riscv_page_map(frame, vspace, v, rights,
                                       QSOE_VM_ATTR_DEFAULT);
            if (err) { sel4_debug_puts("spawn: Page_Map (child) failed\n"); return -ENOMEM; }
        }
    }

    /* 4. IPC buffer page — allocate, zero, map into child. */
    seL4_CPtr ipc_frame = alloc_object(seL4_RISCV_4K_Page, 0);
    if (!ipc_frame) return -ENOMEM;
    err = scratch_map(ipc_frame);
    if (err) return -ENOMEM;
    qmemset((void *)TM_SCRATCH_VADDR, 0, 0x1000);
    err = scratch_unmap(ipc_frame);
    if (err) return -ENOMEM;
    err = qsoe_riscv_page_map(ipc_frame, vspace, CHILD_IPC_BUFFER,
                              QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT);
    if (err) { sel4_debug_puts("spawn: ipc_frame Page_Map failed\n"); return -ENOMEM; }

    /* v0.4.4: allocate the stack region below the IPC buffer.
     * CHILD_STACK_PAGES pages cover [CHILD_STACK_BASE, CHILD_STACK_TOP).
     * The top page gets populated with the SysV ABI initial-stack
     * image (argc/argv/envp/auxv/strings) via scratch_map FIRST, then
     * all pages are mapped into the child. (Mapping into the child
     * before the scratch-map would fail with "frame does not belong
     * to passed address space" — a frame may only be mapped in one
     * VSpace at a time.) */
    seL4_CPtr stack_frames[CHILD_STACK_PAGES];
    for (int i = 0; i < CHILD_STACK_PAGES; ++i) {
        stack_frames[i] = alloc_object(seL4_RISCV_4K_Page, 0);
        if (!stack_frames[i]) {
            sel4_debug_puts("spawn: stack frame alloc failed\n");
            return -ENOMEM;
        }
    }
    /* Build the SysV initial-stack image in the top stack page
     * BEFORE mapping it into the child. */
    unsigned long initial_sp =
        build_initial_stack(stack_frames[CHILD_STACK_PAGES - 1],
                            argc, argv, envc, envp);
    if (!initial_sp) {
        sel4_debug_puts("spawn: build_initial_stack failed\n");
        return -E2BIG;
    }
    /* Now map all stack pages into the child. */
    for (int i = 0; i < CHILD_STACK_PAGES; ++i) {
        unsigned long va = CHILD_STACK_BASE + (unsigned long)i * 0x1000UL;
        err = qsoe_riscv_page_map(stack_frames[i], vspace, va,
                                  QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT);
        if (err) {
            sel4_debug_puts("spawn: stack Page_Map failed\n");
            return -ENOMEM;
        }
    }

    /* 5. Populate the child's CSpace. Slot 1 = Send cap to taskman's
     *    primary endpoint, badged with the child's pid. The child's
     *    CNode is freshly retyped — depth = its radix (12), no guard.
     *    The TCB_Configure step below sets a guard that gives the child
     *    a 64-bit effective CSpace at runtime. */
    err = qsoe_cnode_mint(cnode, QSOE_CAP_TASKMAN_EP, 12,
                          s_cnode_root, primary_ep, 64,
                          QSOE_RIGHTS_SEND, (seL4_Word)pid);
    if (err) { sel4_debug_puts("spawn: mint TASKMAN_EP failed\n"); return -ENOMEM; }

    /* 5b. Untyped budget. Retype 256 KiB (2^18) of untyped out of
     *     taskman's pool; copy the resulting Untyped cap into the
     *     child's slot QSOE_CAP_OWN_UNTYPED. v0.4.1 just *establishes*
     *     the budget — libqsoe still goes through taskman for
     *     ChannelCreate. v0.5 will let the child retype from this
     *     directly. */
    seL4_CPtr child_untyped = alloc_object(seL4_UntypedObject, 18);
    if (!child_untyped) {
        sel4_debug_puts("spawn: child untyped retype failed\n");
        return -ENOMEM;
    }
    err = qsoe_cnode_copy(cnode, QSOE_CAP_OWN_UNTYPED, 12,
                          s_cnode_root, child_untyped, 64,
                          QSOE_RIGHTS_ALL);
    if (err) { sel4_debug_puts("spawn: copy OWN_UNTYPED failed\n"); return -ENOMEM; }

    /* 6. Configure the TCB. cnode_data encodes guard size (52 = 64 −
     *    12) and guard value 0; the CNode is 2^12 slots so addresses
     *    fit in 12 bits. seL4_CNode_CapData layout: bits[0..5] =
     *    guardSize, bits[6..63] = guard value. */
    seL4_Word cnode_data = 52UL;  /* guardSize=52, guard=0 */
    err = qsoe_tcb_configure(tcb, 0 /*fault_ep*/,
                              cnode, cnode_data,
                              vspace, 0 /*vspace_data*/,
                              CHILD_IPC_BUFFER, ipc_frame);
    if (err) { sel4_debug_puts("spawn: TCB_Configure failed\n"); return -ENOMEM; }

    /* Spawned processes run below taskman. taskman blocks on Recv when
     * it has no work, so lower-priority threads always get the CPU. */
    err = qsoe_tcb_set_priority(tcb, seL4_CapInitThreadTCB, 254);
    if (err) { sel4_debug_puts("spawn: TCB_SetPriority failed\n"); return -ENOMEM; }

    /* 7. WriteRegisters: pc=e_entry, a0=pid, sp=initial_sp (pointing
     *    at argc in the SysV image we just wrote into the top stack
     *    page). gp=0 because the binary's start.S sets it itself. */
    qsoe_user_ctx_t ctx;
    qmemset(&ctx, 0, sizeof ctx);
    ctx.pc = eh->e_entry;
    ctx.sp = initial_sp;
    ctx.gp = 0;
    ctx.a0 = (seL4_Word)pid;
    err = qsoe_tcb_write_registers(tcb, 0, &ctx);
    if (err) { sel4_debug_puts("spawn: TCB_WriteRegisters failed\n"); return -ENOMEM; }

    /* 8. Register the new process in taskman's process table so the
     *    lifecycle handlers can find its CSpace + slot allocator. */
    int reg_err = tm_process_register(pid, cnode, tcb, vspace,
                                       QSOE_CAP_WELL_KNOWN_END);
    if (reg_err) {
        sel4_debug_puts("spawn: tm_process_register failed\n");
        return reg_err;
    }
    /* Record the child's untyped budget master for cleanup on terminate. */
    tm_process_t *prec = tm_process_lookup(pid);
    if (prec) prec->untyped_budget = child_untyped;

    /* 8b. Register the connection record for the SYSMGR_COID cap we
     *     minted in step 5. The badge we used was `pid` itself, so the
     *     server-side ConnectClientInfo(scoid==pid) will resolve to
     *     this connection. Without this record, ConnectServerInfo /
     *     ConnectFlags on SYSMGR_COID would EBADF in the child. */
    int primary_idx = tm_channel_index(QSOE_PID_TASKMAN, SYSMGR_CHID);
    if (primary_idx < 0) {
        sel4_debug_puts("spawn: primary channel not registered yet\n");
        return -EINVAL;
    }
    int cnreg = tm_connection_register_existing(pid, QSOE_CAP_TASKMAN_EP,
                                                primary_idx,
                                                (seL4_Word)pid, 0);
    if (cnreg) {
        sel4_debug_puts("spawn: tm_connection_register_existing failed\n");
        return cnreg;
    }

    /* 9. Liftoff. */
    err = qsoe_tcb_resume(tcb);
    if (err) { sel4_debug_puts("spawn: TCB_Resume failed\n"); return -ENOMEM; }

    return 0;
}
