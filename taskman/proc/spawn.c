/*
 * spawn.c — minimal user-space process spawner for QSOE.
 *
 * Hand-rolled per plan §3 — no libsel4utils. Operates on a single
 * static "spawn context" since v0.3 only ever spawns one process at a
 * time. The full process-table machinery lives in server.[ch] and gets
 * populated here at the end of a successful spawn.
 */

#include "spawn.h"
#include "../qsoe_invoke.h"
#include "../tm_log.h"
#include "proc.h"
#include "../mem/mem.h"
#include "../path/pathmgr.h"
#include "../path/cpiofs.h"
#include <qsoe/slots.h>
#include <tm_elf.h>
#include <tm_reloc.h>
#include <tm_script.h>

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

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6
#define PF_X    1
#define PF_W    2
#define PF_R    4

/* SysV auxv types we hand rtld -- see libc/rtld/machine/elf.h. */
#define AT_NULL_     0
#define AT_PHDR_     3
#define AT_PHENT_    4
#define AT_PHNUM_    5
#define AT_PAGESZ_   6
#define AT_BASE_     7
#define AT_ENTRY_    9
#define AT_KPRELOAD_ 34

/* Hard pre-load contract for libc.so -- must match <sys/qsoe.h>'s
 * QSOE_LIBC_LOAD_VA.  rtld walks libc.so's .dynsym at this fixed VA
 * to resolve its bootstrap syscalls; the value is QSOE-wide so a
 * single ld-qsoe.so.1 binary serves both NQ and LQ. */
#define DL_LIBC_LOAD_VA   0x60000000UL
/* Where taskman pre-loads the dynamic linker.  Private to spawn.c --
 * rtld receives this base via AT_BASE in the auxv, so no public ABI
 * is implied. */
#define DL_RTLD_LOAD_VA   0x70000000UL

/* Scratch vaddr in taskman's VSpace, used to memcpy ELF bytes into a
 * frame before we Page_Map that frame into the child's VSpace.
 *
 * Placed at 0x40000000 (L2[1], second 1-GiB slot).  The kernel-
 * prepared L1 + L0 PTs only cover the low 2 MiB [0, 0x200000) where
 * taskman's image, BSS, stack, IPC buffer and extra-BI frames live.
 * Putting scratch in L2[1] gives it its own L1 + L0 PT pair that we
 * allocate once at first use (ensure_scratch_pt below), out of
 * reach of any future taskman.elf size growth.  As long as taskman
 * fits in its 1 GiB image cap (see [[project_image_size_cap]]),
 * scratch never has to move again. */
#define TM_SCRATCH_VADDR 0x40000000UL

/* Child VSpace layout. Image, stack, and IPC buffer share the first
 * 2 MiB region [0, 0x200000) and use one L1 + one L0 PT. The heap
 * (v0.5.1+) gets its own 2 MiB Mega_Page at the next L1 slot, mapped
 * at 0x800000. Worker thread regions sit way out at 0x40000000+. */
#define CHILD_IMAGE_BASE   0x10000UL   /* matches tester's linker script */
#define CHILD_TCB_BASE     0x1FB000UL  /* 1 page TLS/TCB block (qsoe_tcb_t) */
#define CHILD_STACK_BASE   0x1FC000UL  /* 2 stack pages: [0x1FC000, 0x1FE000) */
#define CHILD_STACK_TOP    0x1FE000UL  /* sp starts here, grows down */
#define CHILD_STACK_PAGES  2
#define CHILD_IPC_BUFFER   0x1FE000UL  /* one page, just above the stack */

/* seL4 priority for spawned user processes — one below taskman so the
 * server always preempts.  (seL4 priorities run 0..255; taskman, the
 * root task, sits at 255.) */
#define TM_PRIO_USER       254
/* v0.6.4: no pre-allocated heap.  Memory comes on demand via
 * TM_REQ_MMAP — see tm_mmap_serve below.  The bottom of that region
 * is QSOE_MMAP_BASE (= 0x2000000, 32 MiB), well above the image,
 * stack, and IPC buffer that live in [0, 0x200000). */

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

/* Forward decl of the shared spawn-state untyped/cnode-root helpers
 * defined in proc/process.c (and used below to retype PT objects).
 * `alloc_object` is defined further down in this file. */
static seL4_CPtr alloc_object(seL4_Word type, seL4_Word size_bits);

/* Allocate + install the L1 and L0 page tables that back the 2-MiB
 * region containing TM_SCRATCH_VADDR.  The kernel-prepared mappings
 * only cover the low 2 MiB of taskman's vspace; once taskman's
 * image+extras approach that boundary, scratch needs its own PT
 * pair somewhere out of the way.  Idempotent — runs once at first
 * scratch_map() and stays installed for taskman's lifetime. */
static int s_scratch_pt_ready;

static int ensure_scratch_pt(void)
{
    if (s_scratch_pt_ready) return 0;

    /* L1 PT (covers 1 GiB, here the slice containing TM_SCRATCH_VADDR
     * — entry 1 of the SV39 L2 root).  The first Riscv_PageTable_Map
     * at a VA whose L2 entry is empty installs at L1; the second
     * installs at L0 underneath it. */
    seL4_CPtr l1 = alloc_object(seL4_RISCV_PageTableObject, 0);
    if (!l1) { tm_err("spawn: ensure_scratch_pt: L1 retype failed"); return -ENOMEM; }
    seL4_Word err = qsoe_riscv_pagetable_map(l1, seL4_CapInitThreadVSpace,
                                              TM_SCRATCH_VADDR,
                                              QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("spawn: ensure_scratch_pt: L1 PageTable_Map failed err=%u",
               (unsigned long)err);
        return -ENOMEM;
    }

    /* L0 PT (covers 2 MiB; the leaf level under which 4 KiB Pages are
     * mapped).  After this, qsoe_riscv_page_map() at TM_SCRATCH_VADDR
     * will succeed for any frame the spawner hands it. */
    seL4_CPtr l0 = alloc_object(seL4_RISCV_PageTableObject, 0);
    if (!l0) { tm_err("spawn: ensure_scratch_pt: L0 retype failed"); return -ENOMEM; }
    err = qsoe_riscv_pagetable_map(l0, seL4_CapInitThreadVSpace,
                                    TM_SCRATCH_VADDR,
                                    QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("spawn: ensure_scratch_pt: L0 PageTable_Map failed err=%u",
               (unsigned long)err);
        return -ENOMEM;
    }

    s_scratch_pt_ready = 1;
    return 0;
}

/* Map a frame temporarily into taskman's vspace at TM_SCRATCH_VADDR. */
static int scratch_map(seL4_CPtr frame)
{
    if (ensure_scratch_pt() != 0) return -ENOMEM;

    int rc = (int)qsoe_riscv_page_map(frame, seL4_CapInitThreadVSpace,
                                       TM_SCRATCH_VADDR,
                                       QSOE_RIGHTS_ALL,
                                       QSOE_VM_ATTR_DEFAULT);
    if (rc) {
        tm_err("spawn: scratch_map: vaddr=%08x rc=%02x",
               (unsigned long)TM_SCRATCH_VADDR, (unsigned long)rc);
    }
    return rc;
}

static int scratch_unmap(seL4_CPtr frame)
{
    return (int)qsoe_riscv_page_unmap(frame);
}

/* The shared spawn state — defined in proc/process.c, declared
 * extern via proc.h.  No re-declaration needed here. */

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

/* One auxv entry, packed as two 8-byte words for the SysV ABI image. */
struct aux_pair { unsigned long type; unsigned long val; };

/* Build the SysV ABI initial-stack image into the top page of the
 * child's stack region.
 *
 * Layout from high to low (the child sees sp pointing at argc):
 *   [string area: argv[0]\0 argv[1]\0 ... envp[0]\0 envp[1]\0 ...]
 *   [auxv terminator: a_type = AT_NULL = 0, a_un = 0]   16 bytes
 *   [auxv entries auxc..0]                              16 bytes each
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
                                          int envc, const char *const *envp,
                                          const struct aux_pair *auxv,
                                          int auxc)
{
    /* Compute total bytes needed for the string area. */
    unsigned long strs_bytes = 0;
    for (int i = 0; i < argc; ++i) strs_bytes += qstrlen(argv[i]) + 1;
    for (int i = 0; i < envc; ++i) strs_bytes += qstrlen(envp[i]) + 1;

    /* Pointer + terminator area below the strings. */
    unsigned long below = 8 /*argc*/
                        + 8UL * (unsigned long)(argc + 1) /*argv + NULL*/
                        + 8UL * (unsigned long)(envc + 1) /*envp + NULL*/
                        + 16UL * (unsigned long)(auxc + 1); /*auxv + AT_NULL*/

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
    /* AT_NULL terminator first (ends up at the highest auxv address). */
    *--p = 0;                                 /* auxv terminator a_un */
    *--p = AT_NULL_;                          /* auxv terminator a_type */
    /* User-supplied entries in reverse so they land in caller order. */
    for (int i = auxc - 1; i >= 0; --i) {
        *--p = auxv[i].val;
        *--p = auxv[i].type;
    }
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

/* v0.6.1: UART device-untyped slot in taskman's CSpace, set by
 * main.c at boot after scanning BootInfo. spawn.c uses it to grant
 * UART MMIO + IRQHandler + Notification caps to devc-ser8250. */
static seL4_CPtr s_uart_dev_ut;

void tm_set_uart_untyped(seL4_CPtr ut_slot)
{
    s_uart_dev_ut = ut_slot;
}

/* Per-spawn table of (page vaddr, frame cap) pairs recorded as each
 * PT_LOAD page is mapped into the child.  Used by the relocation
 * write callback to find the frame backing a given target VA, scratch-
 * map it, write 8 bytes, and unmap.  Capacity covers qsh + libc.so +
 * rtld (≈122 pages) with headroom. */
#define SPAWN_MAX_FRAMES 192

typedef struct {
    unsigned long va_page;      /* 4 KiB-aligned child VA */
    seL4_CPtr     frame;        /* the frame cap in taskman's CSpace */
} spawn_frame_t;

static spawn_frame_t s_frames[SPAWN_MAX_FRAMES];
static int           s_frame_count;

static int spawn_record_frame(unsigned long va_page, seL4_CPtr frame)
{
    if (s_frame_count >= SPAWN_MAX_FRAMES) {
        tm_err("spawn: frame table overflow (>%d pages)", SPAWN_MAX_FRAMES);
        return -ENOMEM;
    }
    s_frames[s_frame_count].va_page = va_page;
    s_frames[s_frame_count].frame   = frame;
    s_frame_count++;
    return 0;
}

static seL4_CPtr spawn_find_frame(unsigned long va)
{
    unsigned long page = va & ~0xFFFUL;
    for (int i = 0; i < s_frame_count; ++i)
        if (s_frames[i].va_page == page) return s_frames[i].frame;
    return 0;
}

/* seL4 forbids mapping one frame cap in two VSpaces at once.  Since
 * we've already Page_Map'd each frame into the child, we cannot also
 * scratch_map it in taskman's vspace via the same cap.  Workaround:
 * cnode_copy the frame cap into a reusable scratch slot, Page_Map the
 * copy in taskman's vspace, write, Page_Unmap, cnode_delete.  The
 * copy gets its own per-cap mapping state, leaving the original
 * mapping in the child untouched.  Slot is bump-allocated once on
 * first use; the delete after each write resets it for reuse. */
static seL4_CPtr s_reloc_copy_slot;

/* Per-image skip warning -- the user-cookie is the image's short name
 * ("libc.so", "rtld", "main").  Per feedback_stubs_announce we never
 * leave a silent NULL slot: every skipped external goes through the
 * boot log so the next session knows exactly which lq/libc/ stub
 * to add. */
void tm_reloc_skip_warn(void *user, const char *name)
{
    tm_warn("reloc skip: %s leaves NULL slot for %s",
            (const char *)user, name);
}

static int reloc_write_cb(void *user, uint64_t vaddr, uint64_t value)
{
    (void)user;
    seL4_CPtr frame = spawn_find_frame((unsigned long)vaddr);
    if (!frame) {
        tm_err("spawn: reloc target va=%08x has no mapped frame",
               (unsigned long)vaddr);
        return -1;
    }
    if (!s_reloc_copy_slot) {
        if (ensure_scratch_pt() != 0) return -1;
        s_reloc_copy_slot = s_next_slot++;
    }

    seL4_Word err = qsoe_cnode_copy(s_cnode_root, s_reloc_copy_slot, 64,
                                     s_cnode_root, frame, 64,
                                     QSOE_RIGHTS_ALL);
    if (err) {
        tm_err("spawn: reloc cnode_copy failed err=%u", (unsigned long)err);
        return -1;
    }
    err = qsoe_riscv_page_map(s_reloc_copy_slot, seL4_CapInitThreadVSpace,
                              TM_SCRATCH_VADDR, QSOE_RIGHTS_ALL,
                              QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("spawn: reloc Page_Map failed err=%u", (unsigned long)err);
        qsoe_cnode_delete(s_cnode_root, s_reloc_copy_slot, 64);
        return -1;
    }
    unsigned long offset = vaddr & 0xFFFUL;
    *(volatile uint64_t *)(TM_SCRATCH_VADDR + offset) = value;
    __asm__ volatile ("fence rw, rw" ::: "memory");

    qsoe_riscv_page_unmap(s_reloc_copy_slot);
    qsoe_cnode_delete(s_cnode_root, s_reloc_copy_slot, 64);
    return 0;
}

/* Mega_Page scratch VA -- the second 2 MiB slot under the scratch L1
 * PT (which ensure_scratch_pt installs at L2[1]).  L1[1] is unused by
 * the 4-KiB scratch path (whose L0 PT covers L1[0]), so we can install
 * a Mega_Page leaf there directly without conflicting.  Used by
 * tm_spawn_read_args to look in the caller's mmap'd args page. */
#define TM_SCRATCH_MEGA_VADDR  0x40200000UL
static seL4_CPtr s_args_scratch_slot;

/* Copy up to `len` bytes (capped to one 4 KiB page) from the caller's
 * mmap'd args page at `args_va` into a taskman-local buffer.  Used by
 * TM_REQ_SPAWN to pull the packed (path\0 argv\0 envp\0) blob out of
 * the caller's VSpace without giving taskman a permanent mapping.
 *
 *   1. tm_process_find_frame() resolves args_va -> Mega_Page frame cap
 *      (recorded by mmap_anonymous in proc->mmap[]).
 *   2. cnode_copy that frame to a reusable scratch slot (seL4 forbids
 *      mapping one cap in two VSpaces; the copy gets its own state).
 *   3. Page_Map the copy at TM_SCRATCH_MEGA_VADDR (L1[1] leaf).
 *   4. memcpy 4 KiB starting at the caller's in-page offset.
 *   5. Page_Unmap + cnode_delete -- scratch slot ready for the next call.
 *
 * Returns 0 on success, negative errno on failure. */
int tm_spawn_read_args(tm_process_t *proc, unsigned long args_va,
                        unsigned len, void *out_buf)
{
    if (!proc || !out_buf) return -EINVAL;
    if (len == 0 || len > 0x1000) return -EINVAL;

    seL4_CPtr frame = tm_process_find_frame(proc, args_va);
    if (!frame) {
        tm_err("tm_spawn_read_args: pid %d has no mmap covering va=%08x",
               (long)proc->pid, args_va);
        return -EINVAL;
    }

    if (ensure_scratch_pt() != 0) return -ENOMEM;
    if (!s_args_scratch_slot) s_args_scratch_slot = s_next_slot++;

    seL4_Word err = qsoe_cnode_copy(s_cnode_root, s_args_scratch_slot, 64,
                                     s_cnode_root, frame, 64,
                                     QSOE_RIGHTS_ALL);
    if (err) {
        tm_err("tm_spawn_read_args: cnode_copy failed err=%u",
               (unsigned long)err);
        return -ENOMEM;
    }
    err = qsoe_riscv_page_map(s_args_scratch_slot, seL4_CapInitThreadVSpace,
                              TM_SCRATCH_MEGA_VADDR, QSOE_RIGHTS_ALL,
                              QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("tm_spawn_read_args: Page_Map failed err=%u",
               (unsigned long)err);
        qsoe_cnode_delete(s_cnode_root, s_args_scratch_slot, 64);
        return -ENOMEM;
    }

    unsigned long offset = args_va & (QSOE_MEGA_PAGE - 1);
    /* The packed blob fits in one 4 KiB page by contract, so we never
     * need to walk past a page boundary inside the Mega_Page. */
    qmemcpy(out_buf, (const void *)(TM_SCRATCH_MEGA_VADDR + offset), len);
    __asm__ volatile ("fence rw, rw" ::: "memory");

    qsoe_riscv_page_unmap(s_args_scratch_slot);
    qsoe_cnode_delete(s_cnode_root, s_args_scratch_slot, 64);
    return 0;
}

/* Walk PT_LOAD segments of `elf_blob` and map each page into the
 * child VSpace at (load_offset + p_vaddr).  Pages allocated from
 * taskman's untyped via alloc_object; bytes copied through
 * scratch_map.  Each (va_page, frame_cap) pair is recorded in
 * s_frames[] so the later reloc pass can find it.  Used for the main
 * image (load_offset=0 for ET_EXEC), libc.so (load_offset=
 * DL_LIBC_LOAD_VA), and rtld (load_offset=DL_RTLD_LOAD_VA).
 * Assumes any L1/L0 PTs covering the target VAs are already installed
 * -- callers in this file own that.
 *
 * Returns 0 on success, -ENOMEM on any allocation/map failure. */
static int load_elf_segments(seL4_CPtr vspace, const void *elf_blob,
                              unsigned long load_offset)
{
    const struct elf64_hdr  *eh = elf_blob;
    const struct elf64_phdr *ph = (const struct elf64_phdr *)
                                  ((const u8 *)elf_blob + eh->e_phoff);
    seL4_Word err;

    for (u16 i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_LOAD) continue;

        u64 base    = load_offset + ph[i].p_vaddr;
        u64 vstart  = base & ~0xFFFUL;
        u64 vend    = (base + ph[i].p_memsz + 0xFFFUL) & ~0xFFFUL;
        u64 filesz  = ph[i].p_filesz;
        const u8 *src = (const u8 *)elf_blob + ph[i].p_offset;

        for (u64 v = vstart; v < vend; v += 0x1000) {
            seL4_CPtr frame = alloc_object(seL4_RISCV_4K_Page, 0);
            if (!frame) return -ENOMEM;

            int rc = scratch_map(frame);
            if (rc) { tm_err("spawn: load_elf scratch_map failed"); return -ENOMEM; }

            u64 in_page_start = (v < base) ? (base - v) : 0;
            u64 file_off_at_v = (v > base) ? (v - base) : 0;
            u64 file_bytes_this_page = 0;
            if (file_off_at_v < filesz) {
                file_bytes_this_page = filesz - file_off_at_v;
                if (file_bytes_this_page > 0x1000 - in_page_start) {
                    file_bytes_this_page = 0x1000 - in_page_start;
                }
            }

            qmemset((void *)TM_SCRATCH_VADDR, 0, 0x1000);
            if (file_bytes_this_page) {
                qmemcpy((u8 *)TM_SCRATCH_VADDR + in_page_start,
                        src + file_off_at_v, file_bytes_this_page);
            }

            __asm__ volatile ("fence rw, rw" ::: "memory");

            rc = scratch_unmap(frame);
            if (rc) { tm_err("spawn: load_elf scratch_unmap failed"); return -ENOMEM; }

            seL4_CapRights_t rights = seL4_CapRights_new(
                0, 0,
                (ph[i].p_flags & PF_R) ? 1 : 0,
                (ph[i].p_flags & PF_W) ? 1 : 0);
            err = qsoe_riscv_page_map(frame, vspace, v, rights,
                                       QSOE_VM_ATTR_DEFAULT);
            if (err) {
                tm_err("spawn: load_elf Page_Map failed va=%08x", (unsigned long)v);
                return -ENOMEM;
            }
            if (spawn_record_frame((unsigned long)v, frame) != 0)
                return -ENOMEM;
        }
    }
    return 0;
}

/* Match the basename of `a` (everything after the last '/') against
 * the literal `b`.  Lets us key special cases like the devc-ser8250
 * cap-grant on the program name regardless of how it was looked up
 * (bare "devc-ser8250" vs. "/sbin/devc-ser8250"). */
static int spawn_name_eq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    const char *base = a;
    for (const char *p = a; *p; ++p) if (*p == '/') base = p + 1;
    for (unsigned i = 0;; ++i) {
        if (base[i] != b[i]) return 0;
        if (base[i] == 0) return 1;
    }
}

int tm_spawn(const void *elf_blob, unsigned long elf_len,
             pid_t pid, seL4_CPtr primary_ep,
             int argc, const char *const *argv,
             int envc, const char *const *envp,
             const char *elf_name)
{
    /* Reset per-spawn state.  Frame table is rebuilt as PT_LOAD pages
     * are mapped; the reloc walker consults it to find write targets. */
    s_frame_count = 0;

    /* The L1 PT covering [0x40000000, 0x80000000).  In dyn-linked
     * spawns we install it below as `dl_l1` (so libc.so + rtld + the
     * worker region all share L2[1]).  Stashed into the proc record
     * after tm_process_register so ensure_workers_pts() can skip
     * re-installing it — otherwise seL4 rejects with "All objects
     * mapped at this address" when devc-ser8250 et al ask for IRQ
     * workers.  Static spawns leave this NULL and let ensure_workers_pts
     * install both L1 + L0 itself. */
    seL4_CPtr workers_l1_cap = 0;

    /* Shebang handling.  If the blob starts with "#!", look up the
     * interpreter in the CPIO and re-invoke ourselves with it.  Linux
     * argv convention: argv = [interp, optional_arg, script_path,
     * original_argv[1..]].  Recursion limit 1 -- the interpreter must
     * itself be ELF, not another script.  Parsing of the "#!..." line
     * lives in libtaskman (<tm_script.h>) so NQ and LQ share it. */
    {
        /* Static buffers keep new_argv[]'s pointers valid across the
         * tm_spawn recursion call without per-call stack churn. */
        static char interp_path[80];
        static char opt_arg[80];
        unsigned scan_len = (unsigned) (elf_len < 256 ? elf_len : 256);
        if (tm_script_parse_shebang((const uint8_t *) elf_blob, scan_len,
                                    interp_path, sizeof interp_path,
                                    opt_arg, sizeof opt_arg) == 0) {
            if (interp_path[0] != '/') {
                tm_err("spawn: shebang interp must be absolute");
                return -ENOEXEC;
            }
            const char *interp_cpio = interp_path + 1;   /* skip leading '/' */
            int has_arg = (opt_arg[0] != 0);

            unsigned long interp_size = 0;
            const void *interp_blob = tm_cpio_lookup(interp_cpio, &interp_size);
            if (!interp_blob) {
                tm_err("spawn: shebang interpreter not found: %s", interp_cpio);
                return -ENOENT;
            }
            /* Recursion limit: interpreter must itself be ELF. */
            const unsigned char *ib = (const unsigned char *) interp_blob;
            if (interp_size < 4 ||
                ib[0] != 0x7f || ib[1] != 'E' ||
                ib[2] != 'L'  || ib[3] != 'F') {
                tm_err("spawn: nested shebang not supported");
                return -ENOEXEC;
            }

            /* Build new argv: [interp, opt_arg?, script_path, argv[1..]].
             *
             * script_path is the absolute path the interpreter will see
             * for the script: '/' + cpio name (e.g. "/sbin/init").  The
             * shell uses this to open the script through pathmgr — so
             * it must round-trip through cpiofs's resolution. */
            static char script_path[80];
            unsigned long nlen = 0;
            while (elf_name && elf_name[nlen]) ++nlen;
            if (nlen + 2 > sizeof script_path) return -ENOEXEC;
            script_path[0] = '/';
            for (unsigned long i = 0; i < nlen; ++i) {
                script_path[1 + i] = elf_name[i];
            }
            script_path[1 + nlen] = 0;

            static const char *new_argv[16];
            int new_argc = 0;
            new_argv[new_argc++] = interp_path;
            if (has_arg) new_argv[new_argc++] = opt_arg;
            new_argv[new_argc++] = script_path;
            for (int i = 1; i < argc && new_argc < 16; ++i) {
                new_argv[new_argc++] = argv[i];
            }

            return tm_spawn(interp_blob, interp_size, pid, primary_ep,
                            new_argc, new_argv, envc, envp,
                            /*elf_name=*/interp_cpio);
        }
    }

    (void)elf_len;
    const struct elf64_hdr *eh = elf_blob;

    /* Sanity-check the ELF header. */
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F') {
        tm_err("spawn: not an ELF");
        return -EINVAL;
    }
    if (eh->e_ident[4] != 2 /* ELFCLASS64 */) {
        tm_err("spawn: not ELF64");
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
        tm_err("spawn: ASIDPool_Assign failed");
        return -ENOMEM;
    }

    /* 2. Build the child's page-table tree. We need an L1 PT
     *    (covering [0, 1 GiB)) and an L0 PT (covering [0, 2 MiB)).
     *    Sv39 with 4 KiB pages → call PageTable_Map at each
     *    intermediate level. The kernel decides the level from vaddr. */
    seL4_CPtr l1_pt = alloc_object(seL4_RISCV_PageTableObject, 0);
    if (!l1_pt) return -ENOMEM;
    err = qsoe_riscv_pagetable_map(l1_pt, vspace, 0, QSOE_VM_ATTR_DEFAULT);
    if (err) { tm_err("spawn: L1 PageTable_Map failed"); return -ENOMEM; }

    seL4_CPtr l0_pt = alloc_object(seL4_RISCV_PageTableObject, 0);
    if (!l0_pt) return -ENOMEM;
    err = qsoe_riscv_pagetable_map(l0_pt, vspace, 0, QSOE_VM_ATTR_DEFAULT);
    if (err) { tm_err("spawn: L0 PageTable_Map failed"); return -ENOMEM; }

    /* 3. Walk PT_LOAD segments of the main image at link VA.
     *    For ET_EXEC like our current binaries, p_vaddr is the final
     *    address; for ET_DYN PIE we'd pass a non-zero load_offset.
     *    Stage-A only spawns ET_EXEC main images, so 0 is correct. */
    const struct elf64_phdr *ph = (const struct elf64_phdr *)
                                  ((const u8 *)elf_blob + eh->e_phoff);
    int load_rc = load_elf_segments(vspace, elf_blob, /*load_offset=*/0);
    if (load_rc != 0) return load_rc;

    /* 3b. Phase 4: dynamic linking.  If the main image has PT_INTERP,
     *     pre-load rtld + libc.so into the child VSpace and arrange
     *     for the child to start in rtld instead of the main image's
     *     entry.
     *
     *     Layout in the child VSpace:
     *       [0x10000 .. 0x46000)   main image (qsh ET_EXEC link VA)
     *       [0x1FC000 .. 0x1FE000) stack
     *       [0x1FE000 .. 0x1FF000) IPC buffer
     *       [0x60000000 .. +2 MiB) libc.so   -- DL_LIBC_LOAD_VA contract
     *       [0x70000000 .. +2 MiB) rtld      -- private, told via AT_BASE
     *
     *     rtld receives the auxv we build below; its _rtld() walks
     *     AT_KPRELOAD to register libc.so as an Obj_Entry (matched
     *     by DT_SONAME = "libc.so" so the main image's DT_NEEDED
     *     resolves to it without a duplicate filesystem load), then
     *     applies relocations to qsh and libc.so, then jumps to qsh's
     *     entry (AT_ENTRY) with the original sp.                    */
    const struct elf64_phdr *interp_ph = 0;
    for (u16 i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type == PT_INTERP) { interp_ph = &ph[i]; break; }
    }
    tm_info("spawn: %s e_type=%u e_phnum=%u interp=%s", elf_name,
            eh->e_type, eh->e_phnum, interp_ph ? "yes" : "no");

    int          dyn_link        = 0;
    unsigned long entry_pc        = eh->e_entry;
    unsigned long main_phdr_va    = 0;
    unsigned long rtld_load_base  = 0;

    if (interp_ph) {
        /* PT_INTERP body is an ASCIIZ path like "/lib/ld-qsoe.so.1".
         * Strip the leading '/' for the cpio (flat) namespace. */
        const char *interp_path = (const char *)elf_blob + interp_ph->p_offset;
        const char *interp_cpio_name = interp_path;
        if (interp_cpio_name[0] == '/') interp_cpio_name++;

        unsigned long rtld_size = 0;
        const void   *rtld_blob = tm_cpio_lookup(interp_cpio_name, &rtld_size);
        if (!rtld_blob) {
            tm_err("spawn: rtld not in cpio: %s", interp_cpio_name);
            return -ENOENT;
        }

        unsigned long libc_size = 0;
        const void   *libc_blob = tm_cpio_lookup("lib/libc.so", &libc_size);
        if (!libc_blob) {
            tm_err("spawn: lib/libc.so not in cpio");
            return -ENOENT;
        }

        /* Install the page-table tree covering [0x40000000, 0x80000000).
         * One L1 PT (for L2[1]), plus one L0 PT for each of the two
         * 2 MiB regions holding libc.so and rtld.  seL4 picks the
         * level from vaddr + what's already installed. */
        seL4_CPtr dl_l1 = alloc_object(seL4_RISCV_PageTableObject, 0);
        if (!dl_l1) return -ENOMEM;
        err = qsoe_riscv_pagetable_map(dl_l1, vspace, DL_LIBC_LOAD_VA,
                                        QSOE_VM_ATTR_DEFAULT);
        if (err) { tm_err("spawn: DL L1 PT map failed"); return -ENOMEM; }
        /* Same PT object also covers the worker region at 0x40000000 —
         * record it so ensure_workers_pts() doesn't try to install a
         * second L1 PT into the same already-populated L2[1] slot. */
        workers_l1_cap = dl_l1;

        seL4_CPtr libc_l0 = alloc_object(seL4_RISCV_PageTableObject, 0);
        if (!libc_l0) return -ENOMEM;
        err = qsoe_riscv_pagetable_map(libc_l0, vspace, DL_LIBC_LOAD_VA,
                                        QSOE_VM_ATTR_DEFAULT);
        if (err) { tm_err("spawn: libc L0 PT map failed"); return -ENOMEM; }

        seL4_CPtr rtld_l0 = alloc_object(seL4_RISCV_PageTableObject, 0);
        if (!rtld_l0) return -ENOMEM;
        err = qsoe_riscv_pagetable_map(rtld_l0, vspace, DL_RTLD_LOAD_VA,
                                        QSOE_VM_ATTR_DEFAULT);
        if (err) { tm_err("spawn: rtld L0 PT map failed"); return -ENOMEM; }

        /* PT_LOAD-walk libc.so at the fixed VA, then rtld at its base. */
        load_rc = load_elf_segments(vspace, libc_blob, DL_LIBC_LOAD_VA);
        if (load_rc != 0) { tm_err("spawn: libc.so load failed"); return load_rc; }

        load_rc = load_elf_segments(vspace, rtld_blob, DL_RTLD_LOAD_VA);
        if (load_rc != 0) { tm_err("spawn: rtld load failed"); return load_rc; }

        /* 3c. Pre-apply relocations in taskman (mirrors NQ's loader.c
         * approach) so the user-mode rtld walks a pre-relocated world.
         *
         * Order:
         *   1. libc.so first -- internal R_RISCV_RELATIVE entries get
         *      bias added; cross-image references stay unresolved
         *      because no resolver is plumbed in yet (libc.so itself
         *      has no DT_NEEDED).  Then build a resolver from libc.so's
         *      dynsym for the next two passes.
         *   2. rtld next -- -Bsymbolic-linked, so its relocs are all
         *      internal RELATIVE entries that resolve via bias alone.
         *      Still gets libc.so as ext as a safety net.
         *   3. Main image (qsh) last -- its JUMP_SLOT entries resolve
         *      to runtime addresses inside libc.so via the resolver.
         *
         * Each pass: parse the file blob into a tm_elf_view_t, then
         * call tm_reloc_apply with reloc_write_cb scratch-mapping
         * frames in the child VSpace. */
        tm_elf_view_t libc_view, rtld_view, main_view;
        if (tm_elf_parse(libc_blob, libc_size, &libc_view) != 0) {
            tm_err("spawn: libc.so re-parse failed");
            return -ENOEXEC;
        }
        if (tm_elf_parse(rtld_blob, rtld_size, &rtld_view) != 0) {
            tm_err("spawn: rtld re-parse failed");
            return -ENOEXEC;
        }
        if (tm_elf_parse(elf_blob, elf_len, &main_view) != 0) {
            tm_err("spawn: main image re-parse failed");
            return -ENOEXEC;
        }

        /* Per-skip logger per feedback_stubs_announce: silent NULL
         * slots crash hours later with no context.  Surface every
         * unresolved external at load time -- the boot trace then
         * tells us exactly which lq/libc/ stub to add. */
        extern void tm_reloc_skip_warn(void *user, const char *name);

        unsigned long ap = 0, tot = 0, sk = 0;
        if (tm_reloc_apply(&libc_view, DL_LIBC_LOAD_VA, /*ext=*/0,
                            reloc_write_cb, tm_reloc_skip_warn,
                            (void *)"libc.so",
                            &ap, &tot, &sk) != 0) {
            tm_err("spawn: libc.so reloc failed");
            return -ENOEXEC;
        }
        tm_info("spawn: libc.so relocs %lu/%lu (%lu skipped)", ap, tot, sk);

        tm_reloc_resolver_t libc_resolver;
        if (tm_reloc_init_resolver(&libc_view, DL_LIBC_LOAD_VA,
                                    &libc_resolver) != 0) {
            tm_err("spawn: libc.so resolver init failed");
            return -ENOEXEC;
        }

        if (tm_reloc_apply(&rtld_view, DL_RTLD_LOAD_VA, &libc_resolver,
                            reloc_write_cb, tm_reloc_skip_warn,
                            (void *)"rtld",
                            &ap, &tot, &sk) != 0) {
            tm_err("spawn: rtld reloc failed");
            return -ENOEXEC;
        }
        tm_info("spawn: rtld relocs %lu/%lu (%lu skipped)", ap, tot, sk);

        if (tm_reloc_apply(&main_view, /*bias=*/0, &libc_resolver,
                            reloc_write_cb, tm_reloc_skip_warn,
                            (void *)"main",
                            &ap, &tot, &sk) != 0) {
            tm_err("spawn: main reloc failed");
            return -ENOEXEC;
        }
        tm_info("spawn: main relocs %lu/%lu (%lu skipped)", ap, tot, sk);

        /* AT_PHDR is the VA of the main image's program-header table.
         * The PHDR table sits in the first PT_LOAD; compute its VA as
         * (first_load.p_vaddr - first_load.p_offset) + e_phoff. */
        for (u16 i = 0; i < eh->e_phnum; ++i) {
            if (ph[i].p_type != PT_LOAD) continue;
            if (eh->e_phoff >= ph[i].p_offset &&
                eh->e_phoff <  ph[i].p_offset + ph[i].p_filesz) {
                main_phdr_va = ph[i].p_vaddr +
                               (eh->e_phoff - ph[i].p_offset);
                break;
            }
        }

        rtld_load_base = DL_RTLD_LOAD_VA;
        const struct elf64_hdr *rtld_eh = rtld_blob;
        /* NQ pattern: skip rtld and jump straight to the user image's
         * entry.  Taskman's pre-reloc pass already resolved every
         * R_RISCV_RELATIVE / R_RISCV_64 / R_RISCV_JUMP_SLOT in qsh,
         * libc.so, and rtld -- letting rtld re-run relocate_objects
         * would DOUBLE the bias on libc.so's GOT/PLT (target +
         * 2*0x60000000 instead of target + 0x60000000) and jump into
         * unmapped memory at the first call.  rtld_load_base is
         * still recorded so the AT_BASE auxv entry tells curious
         * libc code where rtld lives; rtld is just dormant.  When
         * dlopen() lands we revisit -- at that point taskman won't
         * have done the new image's relocs and rtld needs to step
         * back into the picture. */
        (void)rtld_eh;
        entry_pc       = eh->e_entry;
        dyn_link       = 1;

        const unsigned char *rb = (const unsigned char *)rtld_blob;
        tm_info("spawn: skip rtld magic=%02x%02x%02x%02x rtld_entry=%08x pc=%08x phdr_va=%08x",
                rb[0], rb[1], rb[2], rb[3],
                (unsigned long)rtld_eh->e_entry,
                (unsigned long)entry_pc,
                (unsigned long)main_phdr_va);
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
    if (err) { tm_err("spawn: ipc_frame Page_Map failed"); return -ENOMEM; }

    /* 4b. TCB page -- one zeroed 4 KiB page at CHILD_TCB_BASE that the
     *     thread's tp register will point at.  libc.so / rtld use
     *     `tp + offset` to read/write TLS slots (qsoe_errno at offset
     *     4, qsoe_self_pid further along, etc.).  Without this, the
     *     very first `*tp = ...` from libc (e.g. write() setting
     *     errno) faults on a NULL deref.  Mirrors NQ's TCB-below-stack
     *     pattern (see nq/taskman/sys/spawn.c).
     *
     *     Zero-init is enough for the libc seam: tid=0, qsoe_errno=0,
     *     and the rest defaulted; libc_init refines on first
     *     syscall. */
    seL4_CPtr tcb_frame = alloc_object(seL4_RISCV_4K_Page, 0);
    if (!tcb_frame) return -ENOMEM;
    err = scratch_map(tcb_frame);
    if (err) return -ENOMEM;
    qmemset((void *)TM_SCRATCH_VADDR, 0, 0x1000);
    err = scratch_unmap(tcb_frame);
    if (err) return -ENOMEM;
    err = qsoe_riscv_page_map(tcb_frame, vspace, CHILD_TCB_BASE,
                              QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT);
    if (err) { tm_err("spawn: tcb_frame Page_Map failed"); return -ENOMEM; }

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
            tm_err("spawn: stack frame alloc failed");
            return -ENOMEM;
        }
    }
    /* Build the SysV initial-stack image in the top stack page
     * BEFORE mapping it into the child.  For dynamically-linked
     * programs hand rtld the auxv entries it asserts on: AT_PHDR /
     * AT_PHENT / AT_PHNUM / AT_BASE / AT_ENTRY / AT_PAGESZ plus
     * AT_KPRELOAD = DL_LIBC_LOAD_VA so load_kpreload() picks up
     * libc.so under its DT_SONAME = "libc.so" alias. */
    struct aux_pair auxv[8];
    int auxc = 0;
    if (dyn_link) {
        auxv[auxc++] = (struct aux_pair){ AT_PHDR_,     main_phdr_va };
        auxv[auxc++] = (struct aux_pair){ AT_PHENT_,    sizeof(struct elf64_phdr) };
        auxv[auxc++] = (struct aux_pair){ AT_PHNUM_,    eh->e_phnum };
        auxv[auxc++] = (struct aux_pair){ AT_BASE_,     rtld_load_base };
        auxv[auxc++] = (struct aux_pair){ AT_ENTRY_,    eh->e_entry };
        auxv[auxc++] = (struct aux_pair){ AT_PAGESZ_,   0x1000 };
        auxv[auxc++] = (struct aux_pair){ AT_KPRELOAD_, DL_LIBC_LOAD_VA };
    }

    unsigned long initial_sp =
        build_initial_stack(stack_frames[CHILD_STACK_PAGES - 1],
                            argc, argv, envc, envp, auxv, auxc);
    if (!initial_sp) {
        tm_err("spawn: build_initial_stack failed");
        return -E2BIG;
    }
    /* Now map all stack pages into the child. */
    for (int i = 0; i < CHILD_STACK_PAGES; ++i) {
        unsigned long va = CHILD_STACK_BASE + (unsigned long)i * 0x1000UL;
        err = qsoe_riscv_page_map(stack_frames[i], vspace, va,
                                  QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("spawn: stack Page_Map failed");
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
    if (err) { tm_err("spawn: mint TASKMAN_EP failed"); return -ENOMEM; }

    /* 5b. Untyped budget. Retype 256 KiB (2^18) of untyped out of
     *     taskman's pool; copy the resulting Untyped cap into the
     *     child's slot QSOE_CAP_OWN_UNTYPED. v0.4.1 just *establishes*
     *     the budget — libqsoe still goes through taskman for
     *     ChannelCreate. v0.5 will let the child retype from this
     *     directly. */
    seL4_CPtr child_untyped = alloc_object(seL4_UntypedObject, 18);
    if (!child_untyped) {
        tm_err("spawn: child untyped retype failed");
        return -ENOMEM;
    }
    err = qsoe_cnode_copy(cnode, QSOE_CAP_OWN_UNTYPED, 12,
                          s_cnode_root, child_untyped, 64,
                          QSOE_RIGHTS_ALL);
    if (err) { tm_err("spawn: copy OWN_UNTYPED failed"); return -ENOMEM; }

    /* 5b'. v0.6.4: copy the child's own CNode cap into its slot
     *      QSOE_CAP_CNODE_SELF so the child can invoke
     *      seL4_CNode_SaveCaller on its own CSpace from inside —
     *      required for resmgr park-the-caller patterns
     *      (devc-ser8250 RX). */
    err = qsoe_cnode_copy(cnode, QSOE_CAP_CNODE_SELF, 12,
                          s_cnode_root, cnode, 64,
                          QSOE_RIGHTS_ALL);
    if (err) { tm_err("spawn: copy CNODE_SELF failed"); return -ENOMEM; }

    /* 4c. v0.6.4: no pre-allocated heap.  Memory comes on demand via
     * TM_REQ_MMAP after the child runs.  See tm_mmap_serve below. */

    /* 5c. v0.5.0/v0.6.1: stdio inheritance. Resolve the CURRENT
     *     /dev/console binding via the path manager — early in boot
     *     this is (taskman, TM_CONSOLE_CHID, in-taskman handler);
     *     after init runs pathmgr_repath it points at the real UART
     *     driver's channel. Mint three badged Send-caps on whatever
     *     channel master is currently registered, then record each
     *     connection in taskman's table. */
    tm_pathmgr_obj_t console_obj;
    unsigned cons_consumed = 0;
    if (tm_pathmgr_resolve("/dev/console", &console_obj, &cons_consumed) != 0) {
        tm_err("spawn: /dev/console not in pathmgr");
        return -EINVAL;
    }
    int console_idx = tm_channel_index(console_obj.server_pid,
                                        console_obj.server_chid);
    if (console_idx < 0) {
        tm_err("spawn: /dev/console channel not registered");
        return -EINVAL;
    }
    seL4_CPtr console_master = tm_channel_master(console_idx);
    if (!console_master) {
        tm_err("spawn: /dev/console master cap missing");
        return -EINVAL;
    }
    static const seL4_CPtr stdio_slots[3] = {
        QSOE_CAP_STDIN_CONNECT,
        QSOE_CAP_STDOUT_CONNECT,
        QSOE_CAP_STDERR_CONNECT,
    };
    for (int i = 0; i < 3; ++i) {
        seL4_Word scoid = tm_alloc_scoid();
        err = qsoe_cnode_mint(cnode, stdio_slots[i], 12,
                              s_cnode_root, console_master, 64,
                              QSOE_RIGHTS_SEND, scoid);
        if (err) {
            tm_err("spawn: mint stdio cap failed");
            return -ENOMEM;
        }
        if (tm_connection_register_existing(pid, stdio_slots[i],
                                             console_idx, scoid, 0) != 0) {
            tm_err("spawn: register stdio connection failed");
            return -ENOMEM;
        }
    }

    /* 5d. v0.6.1: driver-cap inheritance. If this child is the
     *     16550 UART driver, grant it (a) an IRQHandler for PLIC
     *     line 10 minted into child slot QSOE_CAP_IRQ_HANDLER,
     *     (b) a 4 KiB device-untyped covering 0x10000000 copied
     *     into QSOE_CAP_UART_FRAME, (c) a fresh Notification minted
     *     into QSOE_CAP_IRQ_NTFN that the IRQHandler will signal
     *     on each rising edge. Manifest-driven cap granting is
     *     v0.7+; for v0.6.1 the special case is gated by ELF name. */
    if (spawn_name_eq(elf_name, "devc-ser8250")) {
        const seL4_Word PLIC_UART_IRQ = 10;
        const seL4_Word TRIGGER_LEVEL = 0;
        const unsigned long UART_VADDR  = 0xA00000UL;  /* L1 slot 5 */
        if (!s_uart_dev_ut) {
            tm_err("spawn: no UART device untyped registered");
            return -ENODEV;
        }
        /* (1) Allocate an L0 PT for the 2 MiB region containing
         *     the UART vaddr, then attach it under the child's L1. */
        seL4_CPtr uart_l0 = alloc_object(seL4_RISCV_PageTableObject, 0);
        if (!uart_l0) return -ENOMEM;
        err = qsoe_riscv_pagetable_map(uart_l0, vspace, UART_VADDR,
                                        QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("spawn: UART L0 PageTable_Map failed");
            return -ENOMEM;
        }
        /* (2) Retype the UART device-untyped into a 4 KiB frame in
         *     taskman's CSpace. We need the cap here to invoke
         *     Page_Map (the frame must be in our CSpace to be the
         *     invocation target, with the *child's* vspace as the
         *     map target). After mapping we'll mint a copy into the
         *     child's CSpace. */
        seL4_CPtr uart_dev_frame = s_next_slot++;
        err = qsoe_untyped_retype(s_uart_dev_ut, seL4_RISCV_4K_Page, 0,
                                    s_cnode_root, 0, 0,
                                    uart_dev_frame, 1);
        if (err) {
            tm_err("spawn: UART device retype failed");
            return -ENOMEM;
        }
        /* (3) Map the device frame at UART_VADDR in the child. */
        err = qsoe_riscv_page_map(uart_dev_frame, vspace, UART_VADDR,
                                   QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("spawn: UART Page_Map failed");
            return -ENOMEM;
        }
        /* (4) Mint a copy of the frame cap into the child's CSpace
         *     (handy for future revoke / re-map). */
        err = qsoe_cnode_copy(cnode, QSOE_CAP_UART_FRAME, 12,
                               s_cnode_root, uart_dev_frame, 64,
                               QSOE_RIGHTS_ALL);
        if (err) {
            tm_err("spawn: UART frame copy failed");
            return -ENOMEM;
        }
        /* Steps (5)/(6) — the spawn-time pre-mint of an IRQHandler +
         * Notification into QSOE_CAP_IRQ_HANDLER / QSOE_CAP_IRQ_NTFN —
         * were removed (v0.10).  They are the leftover v0.7 "magic-named"
         * IRQ wiring that sys/irq.c's comment says v0.8 superseded:
         * devc-ser8250 now claims its line at runtime via
         * InterruptAttachThread -> TM_REQ_IRQ_ATTACH (tm_irq_attach),
         * which mints a fresh (IRQHandler, Notification) pair into the
         * caller's CSpace.  Pre-minting the IRQHandler here claimed PLIC
         * line 10 first, so the driver's runtime attach failed with
         * "Rejecting request for IRQ 10. Already active." (errno=EBUSY)
         * and the UART never became interrupt-driven.
         *
         * NOTE (for review): the UART-MMIO mapping in steps (1)-(4)
         * above is now ALSO dead — devc-ser8250 maps the 16550 itself
         * via mmap(MAP_PHYS, UART_PHYS) (quser/dev/ser8250/uart.c), so
         * QSOE_CAP_UART_FRAME at 0xA00000 is never read.  Left in place
         * pending confirmation; a follow-up can drop this whole
         * spawn_name_eq("devc-ser8250") block. */
    }

    /* 6. Configure the TCB. cnode_data encodes guard size (52 = 64 −
     *    12) and guard value 0; the CNode is 2^12 slots so addresses
     *    fit in 12 bits. seL4_CNode_CapData layout: bits[0..5] =
     *    guardSize, bits[6..63] = guard value. */
    seL4_Word cnode_data = 52UL;  /* guardSize=52, guard=0 */
    err = qsoe_tcb_configure(tcb,
                              cnode, cnode_data,
                              vspace, 0 /*vspace_data*/,
                              CHILD_IPC_BUFFER, ipc_frame);
    if (err) { tm_err("spawn: TCB_Configure failed"); return -ENOMEM; }

    /* MCS: a TCB cannot run until a scheduling context is bound.  Give
     * the main thread a round-robin SC on core 0 and bind it (along with
     * priority) via SetSchedParams.  Spawned processes run below taskman
     * (TM_PRIO_USER); taskman blocks on Recv when idle, so lower-priority
     * threads always get the CPU. */
    seL4_CPtr sc = tm_sched_context_create(/*core=*/0);
    if (!sc) { tm_err("spawn: sched-context create failed"); return -ENOMEM; }
    err = qsoe_tcb_set_sched_params(tcb, seL4_CapInitThreadTCB,
                                    /*mcp=*/TM_PRIO_USER, /*prio=*/TM_PRIO_USER,
                                    sc, /*fault_ep=*/0);
    if (err) { tm_err("spawn: TCB_SetSchedParams failed"); return -ENOMEM; }

    /* MCS: provision the child's reply object at the well-known slot its
     * libc MsgReceive/MsgReply ride (register a6 / Send target).  Retype
     * straight into the child's CNode (node_depth 0 => cnode is the dest
     * CNode itself), mirroring the IRQ-notification retype above. */
    err = qsoe_untyped_retype(s_untyped, seL4_ReplyObject, 0,
                              cnode, 0, 0, QSOE_CAP_REPLY, 1);
    if (err) { tm_err("spawn: child reply object retype failed"); return -ENOMEM; }

    /* 7. WriteRegisters: pc=entry, a0=pid, sp=initial_sp (pointing
     *    at argc in the SysV image we just wrote into the top stack
     *    page). gp=0 because the binary's start.S sets it itself.
     *
     *    For static binaries entry_pc == eh->e_entry.  For dynamic
     *    binaries it's rtld's .rtld_start; rtld parses the auxv,
     *    relocates qsh + libc.so, then jumps to qsh.e_entry. */
    qsoe_user_ctx_t ctx;
    qmemset(&ctx, 0, sizeof ctx);
    ctx.pc = entry_pc;
    ctx.sp = initial_sp;
    ctx.gp = 0;
    ctx.tp = CHILD_TCB_BASE;     /* points at the zeroed TCB page above */
    ctx.a0 = (seL4_Word)pid;
    err = qsoe_tcb_write_registers(tcb, 0, &ctx);
    if (err) { tm_err("spawn: TCB_WriteRegisters failed"); return -ENOMEM; }

    /* 8. Register the new process in taskman's process table so the
     *    lifecycle handlers can find its CSpace + slot allocator. */
    int reg_err = tm_process_register(pid, cnode, tcb, vspace,
                                       QSOE_CAP_WELL_KNOWN_END);
    if (reg_err) {
        tm_err("spawn: tm_process_register failed");
        return reg_err;
    }
    /* Record the child's untyped budget master for cleanup on terminate. */
    tm_process_t *prec = tm_process_lookup(pid);
    if (prec) prec->untyped_budget = child_untyped;
    /* Hand the dyn-link L1 PT to the worker-region allocator (same
     * L2[1] slot covers libc.so/rtld AND the worker region at 0x40000000). */
    if (prec && workers_l1_cap) prec->workers_l1_pt = workers_l1_cap;

    /* 8b. Register the connection record for the SYSMGR_COID cap we
     *     minted in step 5. The badge we used was `pid` itself, so the
     *     server-side ConnectClientInfo(scoid==pid) will resolve to
     *     this connection. Without this record, ConnectServerInfo /
     *     ConnectFlags on SYSMGR_COID would EBADF in the child. */
    /* The primary channel was registered in main.c with raw chid=1.
     * tm_channel_index matches owner_chid as stored, so we use the
     * raw index here -- NOT the encoded TASKMAN_CHID = (gen<<16)|idx
     * the user-facing ABI exposes (see <sys/qsoe.h>).               */
    int primary_idx = tm_channel_index(QSOE_PID_TASKMAN, /*raw chid*/1);
    tm_info("spawn: probe channel pid=%u chid=1 -> idx=%d",
            (unsigned long)QSOE_PID_TASKMAN, (long)primary_idx);
    if (primary_idx < 0) {
        tm_err("spawn: primary channel not registered yet");
        return -EINVAL;
    }
    int cnreg = tm_connection_register_existing(pid, QSOE_CAP_TASKMAN_EP,
                                                primary_idx,
                                                (seL4_Word)pid, 0);
    if (cnreg) {
        tm_err("spawn: tm_connection_register_existing failed");
        return cnreg;
    }

    /* 9. Liftoff. */
    err = qsoe_tcb_resume(tcb);
    if (err) { tm_err("spawn: TCB_Resume failed"); return -ENOMEM; }

    return 0;
}
