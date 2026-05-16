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
#include "../path/pathmgr.h"
#include "../path/cpiofs.h"
#include "../../libqsoe/include/qsoe/slots.h"

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
 * frame before we Page_Map that frame into the child's VSpace.
 *
 * Constraints — the value MUST be:
 *   1. above taskman.elf's image range AND above the IPC buffer
 *      / BootInfo / extra-BI (DTB) frames the kernel places
 *      immediately after the image (otherwise "vaddr already mapped"),
 *   2. below CHILD_STACK_BASE (0x1FC000),
 *   3. inside the kernel-prepared [0, 0x200000) L0 region (otherwise
 *      FrameMap fails with "missing PT").
 *
 * Growth history:
 *   v0.7  image ≈ 1.4 MB, extras at ~0x180000 → scratch at 0x180000
 *   v0.8  image ≈ 1.5 MB, extras span to ~0x190000 → scratch at 0x1F8000
 *   rc3   image ≈ 1.9 MB (tm_log + sync.c bulk), DTB extras reach
 *         past 0x1F8000 → scratch bumped to 0x1FE000 (the last 4-K
 *         page below the 2-MiB L0 region's top).
 *
 * If taskman ever exceeds ~2 MiB total image+extras, scratch needs a
 * fresh L0 PT mapped at a higher VA (e.g. 0x40000000) — this is the
 * cleanup-when-painful path. */
#define TM_SCRATCH_VADDR 0x1FE000UL

/* Child VSpace layout. Image, stack, and IPC buffer share the first
 * 2 MiB region [0, 0x200000) and use one L1 + one L0 PT. The heap
 * (v0.5.1+) gets its own 2 MiB Mega_Page at the next L1 slot, mapped
 * at 0x800000. Worker thread regions sit way out at 0x40000000+. */
#define CHILD_IMAGE_BASE   0x10000UL   /* matches tester's linker script */
#define CHILD_STACK_BASE   0x1FC000UL  /* 2 stack pages: [0x1FC000, 0x1FE000) */
#define CHILD_STACK_TOP    0x1FE000UL  /* sp starts here, grows down */
#define CHILD_STACK_PAGES  2
#define CHILD_IPC_BUFFER   0x1FE000UL  /* one page, just above the stack */
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

/* Map a frame temporarily into taskman's vspace at TM_SCRATCH_VADDR. */
static int scratch_map(seL4_CPtr frame)
{
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

/* v0.6.1: UART device-untyped slot in taskman's CSpace, set by
 * main.c at boot after scanning BootInfo. spawn.c uses it to grant
 * UART MMIO + IRQHandler + Notification caps to devc-ser8250. */
static seL4_CPtr s_uart_dev_ut;

void tm_set_uart_untyped(seL4_CPtr ut_slot)
{
    s_uart_dev_ut = ut_slot;
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
    /* Shebang handling.  If the blob starts with "#!", look up the
     * interpreter in the CPIO and re-invoke ourselves with it.  Linux
     * argv convention: argv = [interp, optional_arg, script_path,
     * original_argv[1..]].  Recursion limit 1 — the interpreter must
     * itself be ELF, not another script. */
    {
        const unsigned char *b = (const unsigned char *)elf_blob;
        if (elf_len >= 2 && b[0] == '#' && b[1] == '!') {
            unsigned long scan = elf_len < 128 ? elf_len : 128;
            unsigned long eol  = 2;
            while (eol < scan && b[eol] != '\n' && b[eol] != 0) ++eol;

            unsigned long p = 2;
            while (p < eol && (b[p] == ' ' || b[p] == '\t')) ++p;
            unsigned long interp_start = p;
            while (p < eol && b[p] != ' ' && b[p] != '\t') ++p;
            unsigned long interp_end = p;
            if (interp_end == interp_start || b[interp_start] != '/') {
                tm_err("spawn: shebang interp missing or not absolute");
                return -ENOEXEC;
            }
            while (p < eol && (b[p] == ' ' || b[p] == '\t')) ++p;
            unsigned long arg_start = p;
            unsigned long arg_end   = eol;
            while (arg_end > arg_start &&
                   (b[arg_end-1] == ' ' || b[arg_end-1] == '\t')) --arg_end;
            int has_arg = (arg_end > arg_start);

            /* Interpreter path stripped of leading '/' for CPIO lookup. */
            static char interp_cpio[64];
            unsigned long ilen = interp_end - interp_start - 1;
            if (ilen == 0 || ilen >= sizeof interp_cpio) return -ENOEXEC;
            for (unsigned long i = 0; i < ilen; ++i) {
                interp_cpio[i] = (char)b[interp_start + 1 + i];
            }
            interp_cpio[ilen] = 0;

            unsigned long interp_size = 0;
            const void *interp_blob = tm_cpio_lookup(interp_cpio, &interp_size);
            if (!interp_blob) {
                tm_err("spawn: shebang interpreter not found: %s", interp_cpio);
                return -ENOENT;
            }
            /* Recursion limit: interpreter must itself be ELF. */
            const unsigned char *ib = (const unsigned char *)interp_blob;
            if (interp_size < 4 ||
                ib[0] != 0x7f || ib[1] != 'E' ||
                ib[2] != 'L'  || ib[3] != 'F') {
                tm_err("spawn: nested shebang not supported");
                return -ENOEXEC;
            }

            /* Preserve the interpreter path (with leading '/') for new
             * argv[0], and the optional argument, into static buffers. */
            static char interp_path[80];
            unsigned long plen = interp_end - interp_start;
            if (plen >= sizeof interp_path) return -ENOEXEC;
            for (unsigned long i = 0; i < plen; ++i) {
                interp_path[i] = (char)b[interp_start + i];
            }
            interp_path[plen] = 0;

            static char opt_arg[80];
            if (has_arg) {
                unsigned long alen = arg_end - arg_start;
                if (alen >= sizeof opt_arg) return -ENOEXEC;
                for (unsigned long i = 0; i < alen; ++i) {
                    opt_arg[i] = (char)b[arg_start + i];
                }
                opt_arg[alen] = 0;
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
            if (err) { tm_err("spawn: scratch_map failed"); return -ENOMEM; }

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
            if (err) { tm_err("spawn: scratch_unmap failed"); return -ENOMEM; }

            /* Map into the child. Permissions follow the PHDR flags. */
            seL4_CapRights_t rights = seL4_CapRights_new(
                0,
                0,
                (ph[i].p_flags & PF_R) ? 1 : 0,
                (ph[i].p_flags & PF_W) ? 1 : 0);
            err = qsoe_riscv_page_map(frame, vspace, v, rights,
                                       QSOE_VM_ATTR_DEFAULT);
            if (err) { tm_err("spawn: Page_Map (child) failed"); return -ENOMEM; }
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
    if (err) { tm_err("spawn: ipc_frame Page_Map failed"); return -ENOMEM; }

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
     * BEFORE mapping it into the child. */
    unsigned long initial_sp =
        build_initial_stack(stack_frames[CHILD_STACK_PAGES - 1],
                            argc, argv, envc, envp);
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
        /* (5) IRQHandler — mint into child slot QSOE_CAP_IRQ_HANDLER. */
        err = qsoe_irq_control_get(seL4_CapIRQControl,
                                    PLIC_UART_IRQ, TRIGGER_LEVEL,
                                    cnode, QSOE_CAP_IRQ_HANDLER, 12);
        if (err) {
            tm_err("spawn: IRQControl_GetTrigger failed");
            return -ENOMEM;
        }
        /* (6) IRQ Notification — retype from RAM untyped directly
         *     into child's slot QSOE_CAP_IRQ_NTFN. node_depth=0 means
         *     "use cnode as the dest CNode itself"; node_offset is the
         *     slot inside. */
        err = qsoe_untyped_retype(s_untyped, seL4_NotificationObject,
                                    seL4_NotificationBits,
                                    cnode, 0, 0,
                                    QSOE_CAP_IRQ_NTFN, 1);
        if (err) {
            tm_err("spawn: IRQ Notification retype failed");
            return -ENOMEM;
        }
    }

    /* 6. Configure the TCB. cnode_data encodes guard size (52 = 64 −
     *    12) and guard value 0; the CNode is 2^12 slots so addresses
     *    fit in 12 bits. seL4_CNode_CapData layout: bits[0..5] =
     *    guardSize, bits[6..63] = guard value. */
    seL4_Word cnode_data = 52UL;  /* guardSize=52, guard=0 */
    err = qsoe_tcb_configure(tcb, 0 /*fault_ep*/,
                              cnode, cnode_data,
                              vspace, 0 /*vspace_data*/,
                              CHILD_IPC_BUFFER, ipc_frame);
    if (err) { tm_err("spawn: TCB_Configure failed"); return -ENOMEM; }

    /* Spawned processes run below taskman. taskman blocks on Recv when
     * it has no work, so lower-priority threads always get the CPU. */
    err = qsoe_tcb_set_priority(tcb, seL4_CapInitThreadTCB, 254);
    if (err) { tm_err("spawn: TCB_SetPriority failed"); return -ENOMEM; }

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

    /* 8b. Register the connection record for the SYSMGR_COID cap we
     *     minted in step 5. The badge we used was `pid` itself, so the
     *     server-side ConnectClientInfo(scoid==pid) will resolve to
     *     this connection. Without this record, ConnectServerInfo /
     *     ConnectFlags on SYSMGR_COID would EBADF in the child. */
    int primary_idx = tm_channel_index(QSOE_PID_TASKMAN, SYSMGR_CHID);
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
