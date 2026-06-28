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
#include <tm_log.h>
#include "proc.h"
#include "../mem/mem.h"
#include "../path/pathmgr.h"
#include "../path/cpiofs.h"
#include <qsoe/slots.h>
#include <qsoe/sysmap.h>
#include "../sys/sysmap.h"
#include <tm_elf.h>
#include <tm_reloc.h>
#include <tm_script.h>
#include <sys/qsoe.h>          /* ConnectAttach/Detach, ND_LOCAL_NODE      */
#include <qsoe/tm_msgs.h>      /* _IO_*, tm_req_io_*, TM_IO_MAX, TM_PATH_MAX */
#include <tm_pathmgr.h>        /* tm_pathmgr_resolve, PATHMGR_HANDLER_*    */

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
#define PT_GNU_RELRO 0x6474e552u   /* read-only-after-relocation segment */
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
#define CHILD_STACK_BASE   0x1F9000UL  /* 2 stack pages: [0x1F9000, 0x1FB000) */
#define CHILD_STACK_TOP    0x1FB000UL  /* sp starts here, grows down */
#define CHILD_STACK_PAGES  2
#define CHILD_TCB_BASE     0x1FB000UL  /* 1 page TLS/TCB block (qsoe_tcb_t) */
#define CHILD_SYSMAP_BASE  QSOE_SYSMAP_VA  /* read-only 'PSYS' page @0x1FC000 */
#define CHILD_IPC_BUFFER   0x1FE000UL  /* seL4 IPC buffer (fixed; libc seam) */

/* TM_PRIO_USER_DEFAULT (the QNX-default priority a spawned user thread
 * runs at) and the SchedSet ceiling/range live in proc.h. */
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
static unsigned long qstrlen(const char *s);

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
        tm_err("spawn: ensure_scratch_pt: L1 PageTable_Map failed err=%lu",
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
        tm_err("spawn: ensure_scratch_pt: L0 PageTable_Map failed err=%lu",
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
        tm_err("spawn: scratch_map: vaddr=%08lx rc=%02lx",
               (unsigned long)TM_SCRATCH_VADDR, (unsigned long)rc);
    }
    return rc;
}

static int scratch_unmap(seL4_CPtr frame)
{
    return (int)qsoe_riscv_page_unmap(frame);
}

/* ====================================================================
 * Spawn-from-filesystem (proc.h tm_fs_load_t / tm_spawn_fs_*).
 *
 * A binary not in the boot cpio is read off a mounted resmgr (fs-qrv) the
 * way any client would: ConnectAttach to the serving channel, _IO_CONNECT
 * (open), an _IO_READ loop into a scratch window in taskman's OWN VSpace,
 * _IO_CLOSE, detach.  The bytes then feed the normal ELF loader.  No
 * deadlock: the read chain taskman -> fs-qrv -> devb never calls back into
 * taskman.  Mirrors NQ's sys/spawn.c fs_read_image, but uses the in-
 * taskman <sys/qsoe.h> IPC seam + a pp_ut-bracketed megaframe buffer. */

/* The read buffer sits at a free L2 root slot, clear of the image (L2[0])
 * and TM_SCRATCH_VADDR (L2[1], which tm_spawn uses at the same time to copy
 * the image into the child).  A 2 MiB megaframe maps at the L1 level, so
 * this window needs only an L1 PT -- installed once from the master pool so
 * it survives the per-read pp_ut Revoke. */
#define FS_SCRATCH_VADDR   0x80000000UL     /* L2[2] of taskman's vspace    */
#define FS_OPEN_RDONLY     0
#define FS_REPLY_DATA_WORD 4                 /* reply payload at msg[4]      */

static int s_fs_scratch_pt_ready;

static int ensure_fs_scratch_pt(void)
{
    if (s_fs_scratch_pt_ready) return 0;
    /* Allocate the L1 PT with s_cur_pput clear (caller ensures this) so it
     * comes from the master pool, not a per-read pp_ut block -- it must
     * persist for taskman's lifetime, like the TM_SCRATCH_VADDR PTs. */
    seL4_CPtr l1 = taskman_alloc_and_retype(seL4_RISCV_PageTableObject, 0);
    if (!l1) { tm_err("spawn: fs scratch L1 retype failed"); return -ENOMEM; }
    seL4_Word err = qsoe_riscv_pagetable_map(l1, seL4_CapInitThreadVSpace,
                                             FS_SCRATCH_VADDR,
                                             QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("spawn: fs scratch L1 map failed err=%lu", (unsigned long) err);
        return -ENOMEM;
    }
    s_fs_scratch_pt_ready = 1;
    return 0;
}

/* Map megaframes (from the active pp_ut block) until the read window covers
 * [0, need).  Megaframes land at FS_SCRATCH_VADDR + k*2 MiB under the one
 * L1 PT. */
static int fs_grow(tm_fs_load_t *ctx, unsigned long need)
{
    while ((unsigned long) ctx->nmf * QSOE_MEGA_PAGE < need) {
        if (ctx->nmf >= TM_FS_MAX_MF) {
            tm_err("spawn: fs image exceeds %d-megaframe cap", TM_FS_MAX_MF);
            return -EFBIG;
        }
        seL4_CPtr mf = taskman_alloc_and_retype(seL4_RISCV_Mega_Page, 0);
        if (!mf) { tm_err("spawn: fs megaframe retype failed"); return -ENOMEM; }
        seL4_Word err = qsoe_riscv_page_map(
            mf, seL4_CapInitThreadVSpace,
            FS_SCRATCH_VADDR + (unsigned long) ctx->nmf * QSOE_MEGA_PAGE,
            QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("spawn: fs megaframe map failed err=%lu", (unsigned long) err);
            return -ENOMEM;
        }
        ctx->mf[ctx->nmf++] = mf;
    }
    return 0;
}

/* Best-effort _IO_CLOSE so the server frees its per-open handle. */
static void fs_close(int coid)
{
    tm_req_io_hdr_t cl;
    cl.type = _IO_CLOSE;
    cl._reserved[0] = cl._reserved[1] = cl._reserved[2] = cl._reserved[3] = 0;
    (void) tm_msg_call(coid, &cl, (int) sizeof cl);
    ConnectDetach(coid);
}

int tm_spawn_fs_load(const char *path, const void **out_blob,
                     unsigned long *out_size, tm_fs_load_t *ctx)
{
    ctx->nmf = 0;
    ctx->pput_n = 0;

    /* Resolve to the serving resmgr.  Only an external resmgr (a mounted
     * fs) is fed here -- taskman-served paths (cpio/sys/proc) were already
     * tried and missed by the caller. */
    tm_pathmgr_obj_t obj;
    unsigned consumed = 0;
    if (tm_pathmgr_resolve(path, &obj, &consumed) != 0)
        return -ENOENT;
    if (obj.handler_kind != PATHMGR_HANDLER_EXTERNAL || obj.server_pid <= 0)
        return -ENOENT;

    /* L1 PT first, while s_cur_pput is still clear (master-pool, permanent). */
    if (ensure_fs_scratch_pt() != 0)
        return -ENOMEM;

    int coid = ConnectAttach(ND_LOCAL_NODE, obj.server_pid, obj.server_chid,
                             0, 0);
    if (coid < 0) {
        tm_err("spawn: fs ConnectAttach(pid=%d chid=%d) failed",
               (int) obj.server_pid, obj.server_chid);
        return -EIO;
    }

    /* Per-read pp_ut block: read-buffer megaframes draw from it and are
     * reclaimed leak-free by tm_spawn_fs_unload's Revoke. */
    if (tm_pput_spawn_begin(ctx->pput, &ctx->pput_n) != 0) {
        ConnectDetach(coid);
        return -ENOMEM;
    }

    /* _IO_CONNECT: open the file (full resolved path). */
    static unsigned char ob[sizeof(tm_req_io_connect_t) + TM_PATH_MAX];
    tm_req_io_connect_t *oc = (tm_req_io_connect_t *) ob;
    unsigned plen = (unsigned) qstrlen(path);
    if (plen >= TM_PATH_MAX) plen = TM_PATH_MAX - 1;
    oc->type  = _IO_CONNECT;
    oc->plen  = plen;
    oc->flags = FS_OPEN_RDONLY;
    oc->mode  = 0;
    oc->_reserved[0] = 0;
    for (unsigned i = 0; i < plen; ++i)
        ob[sizeof(tm_req_io_connect_t) + i] = (unsigned char) path[i];
    int st = tm_msg_call(coid, ob, (int) (sizeof(tm_req_io_connect_t) + plen));
    if (st != 0) goto fail;          /* server errno (ENOENT, ...) */

    /* _IO_READ loop into the scratch window; a zero-byte read is EOF.
     * CRITICAL: grow the buffer (which issues seL4 retype/map invocations
     * that reuse the IPC buffer) BEFORE each read, never between the read
     * and the copy -- otherwise a fresh-megaframe alloc clobbers the reply
     * data still sitting in msg[]. */
    unsigned long total = 0;
    for (;;) {
        if (fs_grow(ctx, total + TM_IO_MAX) != 0) goto fail;
        tm_req_io_read_t rd;
        rd.type = _IO_READ;
        rd.count = TM_IO_MAX;
        rd._reserved[0] = rd._reserved[1] = rd._reserved[2] = 0;
        st = tm_msg_call(coid, &rd, (int) sizeof rd);
        if (st != 0) { tm_err("spawn: fs read rc=%d at %lu", st, total); goto fail; }
        unsigned long n = qsoe_ipcbuf->msg[0];          /* count @ word0 */
        if (n == 0) break;
        if (n > TM_IO_MAX) n = TM_IO_MAX;
        /* No IPC between here and the copy -- msg[] holds the reply data. */
        unsigned char *dst = (unsigned char *) (FS_SCRATCH_VADDR + total);
        const unsigned char *src =
            (const unsigned char *) &qsoe_ipcbuf->msg[FS_REPLY_DATA_WORD];
        for (unsigned long i = 0; i < n; ++i) dst[i] = src[i];
        total += n;
    }

    fs_close(coid);
    tm_pput_end();          /* clear context so the spawn uses its own block */
    *out_blob = (const void *) FS_SCRATCH_VADDR;
    *out_size = total;
    return 0;

fail:
    fs_close(coid);
    tm_pput_end();
    tm_spawn_fs_unload(ctx);
    return -EIO;
}

void tm_spawn_fs_unload(tm_fs_load_t *ctx)
{
    for (int i = 0; i < ctx->nmf; ++i)
        (void) qsoe_riscv_page_unmap(ctx->mf[i]);
    ctx->nmf = 0;
    if (ctx->pput_n > 0) {
        tm_pput_release_list(ctx->pput, ctx->pput_n);
        ctx->pput_n = 0;
    }
}

/* The shared spawn state — defined in proc/process.c, declared
 * extern via proc.h.  No re-declaration needed here. */

/* Allocate one untyped retype into the next free slot. Returns the
 * slot on success, 0 on failure. */
static seL4_CPtr alloc_object(seL4_Word type, seL4_Word size_bits)
{
    /* Route through the shared retype helper so every spawn object draws
     * from the active per-process untyped (set by tm_pput_spawn_begin in
     * the spawn handler) and is reclaimed wholesale on exit. */
    return taskman_alloc_and_retype(type, size_bits);
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
#define SPAWN_MAX_FRAMES 256

typedef struct {
    unsigned long va_page;      /* 4 KiB-aligned child VA */
    seL4_CPtr     frame;        /* the frame cap in taskman's CSpace */
} spawn_frame_t;

static spawn_frame_t s_frames[SPAWN_MAX_FRAMES];
static int           s_frame_count;

/* Per-spawn page-table caps (child image L1/L0, and the dl L1 + libc/
 * rtld L0 for dynamic binaries).  Like the image frames, the PT OBJECTS
 * die with Revoke(pput) on exit, but their root-CNode SLOTS must be
 * reclaimed -- moved into the objcnode at the end of spawn alongside
 * s_frames[].  Kept separate from s_frames[] because the reloc pass
 * looks those up by VA, whereas PTs have no frame VA.  Without this the
 * ~4 PT slots per spawn leak and the root CNode fills after a few
 * hundred spawns. */
#define SPAWN_MAX_PTS 8
static seL4_CPtr s_pt_slots[SPAWN_MAX_PTS];
static int       s_pt_count;

static int spawn_record_pt(seL4_CPtr pt)
{
    if (!pt) return 0;
    if (s_pt_count >= SPAWN_MAX_PTS) {
        tm_err("spawn: PT slot table overflow (>%d)", SPAWN_MAX_PTS);
        return -ENOMEM;
    }
    s_pt_slots[s_pt_count++] = pt;
    return 0;
}

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

/* Per-spawn GNU_RELRO ranges, one per loaded object that carries the
 * segment (main image, libc.so, rtld).  Recorded by load_elf_segments
 * (load_offset already applied); consulted at the objcnode-move step so
 * pages in a range keep an invokeable frame cap for runtime mprotect. */
#define SPAWN_MAX_RELRO 8
typedef struct { unsigned long lo; unsigned long hi; } spawn_relro_t;
static spawn_relro_t s_relro[SPAWN_MAX_RELRO];
static int           s_relro_count;

static int va_in_relro(unsigned long va_page)
{
    for (int i = 0; i < s_relro_count; ++i)
        if (va_page >= s_relro[i].lo && va_page < s_relro[i].hi) return 1;
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
        tm_err("spawn: reloc target va=%08lx has no mapped frame",
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
        tm_err("spawn: reloc cnode_copy failed err=%lu", (unsigned long)err);
        return -1;
    }
    err = qsoe_riscv_page_map(s_reloc_copy_slot, seL4_CapInitThreadVSpace,
                              TM_SCRATCH_VADDR, QSOE_RIGHTS_ALL,
                              QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("spawn: reloc Page_Map failed err=%lu", (unsigned long)err);
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
        tm_err("tm_spawn_read_args: pid %ld has no mmap covering va=%08lx",
               (long)proc->pid, args_va);
        return -EINVAL;
    }

    if (ensure_scratch_pt() != 0) return -ENOMEM;
    if (!s_args_scratch_slot) s_args_scratch_slot = s_next_slot++;

    seL4_Word err = qsoe_cnode_copy(s_cnode_root, s_args_scratch_slot, 64,
                                     s_cnode_root, frame, 64,
                                     QSOE_RIGHTS_ALL);
    if (err) {
        tm_err("tm_spawn_read_args: cnode_copy failed err=%lu",
               (unsigned long)err);
        return -ENOMEM;
    }
    err = qsoe_riscv_page_map(s_args_scratch_slot, seL4_CapInitThreadVSpace,
                              TM_SCRATCH_MEGA_VADDR, QSOE_RIGHTS_ALL,
                              QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("tm_spawn_read_args: Page_Map failed err=%lu",
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

/* Zero a free (currently-unmapped) Mega_Page frame, so a megapage
 * recycled by tm_munmap_serve preserves MAP_ANONYMOUS's zero-fill
 * guarantee (a freshly-retyped frame is zeroed by seL4, a reused one
 * is not).  The frame is mapped alone into taskman's vspace at the
 * Mega_Page scratch VA -- no cnode_copy, unlike tm_spawn_read_args,
 * because a free frame is not mapped in any child vspace -- memset to
 * zero, then unmapped, leaving it ready to Page_Map into the caller.
 * Returns 0 on success, negative errno on failure. */
int tm_zero_megaframe(seL4_CPtr frame)
{
    if (!frame) return -EINVAL;
    if (ensure_scratch_pt() != 0) return -ENOMEM;

    seL4_Word err = qsoe_riscv_page_map(frame, seL4_CapInitThreadVSpace,
                                        TM_SCRATCH_MEGA_VADDR,
                                        QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("tm_zero_megaframe: Page_Map failed err=%lu",
               (unsigned long)err);
        return -ENOMEM;
    }
    qmemset((void *)TM_SCRATCH_MEGA_VADDR, 0, QSOE_MEGA_PAGE);
    __asm__ volatile ("fence rw, rw" ::: "memory");
    qsoe_riscv_page_unmap(frame);
    return 0;
}

/* ---- v0.13 bulk-IPC bounce copy (doc/plans/bulk_ipc.txt) ------------- */

/* A SECOND 2 MiB scratch VA so one copy can map the source and the
 * destination frame at the same time (TM_SCRATCH_MEGA_VADDR is the
 * source slot, shared with tm_spawn_read_args / tm_zero_megaframe).
 * Both VAs sit under the scratch L1 PT that ensure_scratch_pt() installs
 * for the L2[1] gigabyte, so no extra page-table setup is needed. */
#define TM_SCRATCH_MEGA_VADDR_B  0x40400000UL   /* 2nd 2 MiB scratch slot */

/* Hard ceiling on a single bulk transfer (mirrors NQ's bulk IPC cap). */
#define TM_MSG_BULK_MAX          (16UL * 1024 * 1024)

/* cnode depth of taskman's flat root CNode (matches tm_spawn_read_args). */
#define TM_BULK_CNODE_DEPTH      64

static seL4_CPtr s_bulk_slot_src;   /* scratch cap slot for the source frame */
static seL4_CPtr s_bulk_slot_dst;   /* scratch cap slot for the dest   frame */

long tm_bulk_copy(tm_process_t *src_proc, unsigned long src_va,
                  tm_process_t *dst_proc, unsigned long dst_va,
                  unsigned long len)
{
    if (!src_proc || !dst_proc) return -EINVAL;
    if (len == 0) return 0;
    if (len > TM_MSG_BULK_MAX) return -E2BIG;
    if (ensure_scratch_pt() != 0) return -ENOMEM;
    if (!s_bulk_slot_src) s_bulk_slot_src = s_next_slot++;
    if (!s_bulk_slot_dst) s_bulk_slot_dst = s_next_slot++;

    unsigned long done = 0;
    while (done < len) {
        unsigned long sva = src_va + done;
        unsigned long dva = dst_va + done;
        seL4_CPtr src_cnode, src_slot;
        seL4_Uint8 src_depth;
        int src_is_mega;
        seL4_CPtr dst_cnode, dst_slot;
        seL4_Uint8 dst_depth;
        int dst_is_mega;

        if (tm_process_resolve_frame(src_proc, sva, &src_cnode, &src_slot, &src_depth, &src_is_mega) != 0 ||
            tm_process_resolve_frame(dst_proc, dva, &dst_cnode, &dst_slot, &dst_depth, &dst_is_mega) != 0) {
            tm_err("tm_bulk_copy: unmapped VA (src pid %ld va=%08lx -> %lu, "
                   "dst pid %ld va=%08lx -> %lu)",
                   (long)src_proc->pid, sva, 0UL,
                   (long)dst_proc->pid, dva, 0UL);
            return -EFAULT;
        }

        unsigned long src_page_size = src_is_mega ? QSOE_MEGA_PAGE : QSOE_PAGE_4K;
        unsigned long dst_page_size = dst_is_mega ? QSOE_MEGA_PAGE : QSOE_PAGE_4K;

        unsigned long so  = sva & (src_page_size - 1);
        unsigned long dof = dva & (dst_page_size - 1);

        unsigned long chunk = len - done;
        if (chunk > src_page_size - so)  chunk = src_page_size - so;
        if (chunk > dst_page_size - dof) chunk = dst_page_size - dof;

        seL4_Word err = qsoe_cnode_copy(s_cnode_root, s_bulk_slot_src,
                                         TM_BULK_CNODE_DEPTH, src_cnode, src_slot,
                                         src_depth, QSOE_RIGHTS_ALL);
        if (err) return -ENOMEM;
        err = qsoe_cnode_copy(s_cnode_root, s_bulk_slot_dst,
                              TM_BULK_CNODE_DEPTH, dst_cnode, dst_slot,
                              dst_depth, QSOE_RIGHTS_ALL);
        if (err) {
            qsoe_cnode_delete(s_cnode_root, s_bulk_slot_src, TM_BULK_CNODE_DEPTH);
            return -ENOMEM;
        }

        unsigned long src_scratch_va = src_is_mega ? TM_SCRATCH_MEGA_VADDR : TM_SCRATCH_VADDR;
        unsigned long dst_scratch_va = dst_is_mega ? TM_SCRATCH_MEGA_VADDR_B : (TM_SCRATCH_VADDR + 0x1000UL);

        err = qsoe_riscv_page_map(s_bulk_slot_src, seL4_CapInitThreadVSpace,
                                  src_scratch_va, QSOE_RIGHTS_ALL,
                                  QSOE_VM_ATTR_DEFAULT);
        if (!err)
            err = qsoe_riscv_page_map(s_bulk_slot_dst, seL4_CapInitThreadVSpace,
                                      dst_scratch_va, QSOE_RIGHTS_ALL,
                                      QSOE_VM_ATTR_DEFAULT);
        if (err) {
            qsoe_riscv_page_unmap(s_bulk_slot_src);
            qsoe_cnode_delete(s_cnode_root, s_bulk_slot_src, TM_BULK_CNODE_DEPTH);
            qsoe_cnode_delete(s_cnode_root, s_bulk_slot_dst, TM_BULK_CNODE_DEPTH);
            return -ENOMEM;
        }

        qmemcpy((void *)(dst_scratch_va + dof),
                (const void *)(src_scratch_va + so), chunk);
        __asm__ volatile ("fence rw, rw" ::: "memory");

        qsoe_riscv_page_unmap(s_bulk_slot_src);
        qsoe_riscv_page_unmap(s_bulk_slot_dst);
        qsoe_cnode_delete(s_cnode_root, s_bulk_slot_src, TM_BULK_CNODE_DEPTH);
        qsoe_cnode_delete(s_cnode_root, s_bulk_slot_dst, TM_BULK_CNODE_DEPTH);
        done += chunk;
    }
    return (long)done;
}

/* Per-client stash of the blocked sender's reply buffer, recorded at
 * PULL and consumed at PUSH.  A client is blocked in exactly one MsgSend
 * at a time, so keying by client pid is unambiguous. */
#define TM_BULK_PENDING_MAX  16
static struct {
    int           in_use;
    pid_t         client_pid;
    unsigned long rbuf_va;
    unsigned long rbytes;
} s_bulk_pending[TM_BULK_PENDING_MAX];

static int bulk_pending_find(pid_t client_pid)
{
    for (int i = 0; i < TM_BULK_PENDING_MAX; ++i)
        if (s_bulk_pending[i].in_use && s_bulk_pending[i].client_pid == client_pid)
            return i;
    return -1;
}

long tm_msg_xfer_pull(pid_t server_pid, pid_t client_pid,
                      unsigned long client_src_va, unsigned long server_dst_va,
                      unsigned long len, unsigned long client_rbuf_va,
                      unsigned long client_rbytes)
{
    tm_process_t *client = tm_process_lookup(client_pid);
    tm_process_t *server = tm_process_lookup(server_pid);
    if (!client || !server) return -ESRCH;

    long r = tm_bulk_copy(client, client_src_va, server, server_dst_va, len);
    if (r < 0) return r;

    int slot = bulk_pending_find(client_pid);
    if (slot < 0) {
        for (int i = 0; i < TM_BULK_PENDING_MAX; ++i)
            if (!s_bulk_pending[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -EAGAIN;       /* too many concurrent bulk receives */
    s_bulk_pending[slot].in_use     = 1;
    s_bulk_pending[slot].client_pid = client_pid;
    s_bulk_pending[slot].rbuf_va    = client_rbuf_va;
    s_bulk_pending[slot].rbytes     = client_rbytes;
    return r;
}

long tm_msg_xfer_push(pid_t server_pid, pid_t client_pid,
                      unsigned long server_src_va, unsigned long len)
{
    int slot = bulk_pending_find(client_pid);
    if (slot < 0) return -ESRCH;        /* no matching PULL on record */

    tm_process_t *client = tm_process_lookup(client_pid);
    tm_process_t *server = tm_process_lookup(server_pid);
    if (!client || !server) { s_bulk_pending[slot].in_use = 0; return -ESRCH; }

    unsigned long n = len;
    if (n > s_bulk_pending[slot].rbytes) n = s_bulk_pending[slot].rbytes;
    long r = 0;
    if (n > 0)
        r = tm_bulk_copy(server, server_src_va, client,
                         s_bulk_pending[slot].rbuf_va, n);
    s_bulk_pending[slot].in_use = 0;    /* one reply per blocked send */
    return r;
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
                tm_err("spawn: load_elf Page_Map failed va=%08lx", (unsigned long)v);
                return -ENOMEM;
            }
            if (spawn_record_frame((unsigned long)v, frame) != 0)
                return -ENOMEM;
        }
    }

    /* Note this object's GNU_RELRO range (load_offset applied) so the
     * objcnode-move step keeps those pages' frame caps invokeable for
     * rtld's runtime mprotect(PROT_READ). */
    for (u16 i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != PT_GNU_RELRO) continue;
        unsigned long lo = (load_offset + ph[i].p_vaddr) & ~0xFFFUL;
        unsigned long hi = (load_offset + ph[i].p_vaddr + ph[i].p_memsz
                            + 0xFFFUL) & ~0xFFFUL;
        if (s_relro_count >= SPAWN_MAX_RELRO) {
            tm_err("spawn: RELRO range table full (>%d)", SPAWN_MAX_RELRO);
            break;
        }
        s_relro[s_relro_count].lo = lo;
        s_relro[s_relro_count].hi = hi;
        s_relro_count++;
    }
    return 0;
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
    s_pt_count    = 0;
    s_relro_count = 0;

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
    if (spawn_record_pt(l1_pt) != 0) return -ENOMEM;
    err = qsoe_riscv_pagetable_map(l1_pt, vspace, 0, QSOE_VM_ATTR_DEFAULT);
    if (err) { tm_err("spawn: L1 PageTable_Map failed"); return -ENOMEM; }

    seL4_CPtr l0_pt = alloc_object(seL4_RISCV_PageTableObject, 0);
    if (!l0_pt) return -ENOMEM;
    if (spawn_record_pt(l0_pt) != 0) return -ENOMEM;
    err = qsoe_riscv_pagetable_map(l0_pt, vspace, 0, QSOE_VM_ATTR_DEFAULT);
    if (err) { tm_err("spawn: L0 PageTable_Map failed"); return -ENOMEM; }

    /* 3. Walk PT_LOAD segments of the main image at link VA.
     *    For ET_EXEC like our current binaries, p_vaddr is the final
     *    address; for ET_DYN PIE we'd pass a non-zero load_offset.
     *    We only spawn ET_EXEC main images, so 0 is correct. */
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
     *       [0x1F9000 .. 0x1FB000) stack
     *       [0x1FB000 .. 0x1FC000) TLS/TCB page
     *       [0x1FC000 .. 0x1FD000) sysmap (read-only PSYS, QSOE_SYSMAP_VA)
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
    tm_dbg("spawn: %s e_type=%u e_phnum=%u interp=%s", elf_name,
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

        /* dl_l1 above is NOT recorded for objcnode-move: it doubles as
         * workers_l1_cap -> workers_l1_pt, whose slot teardown step 7
         * already frees.  The two L0s below have no such alias, so their
         * slots must be reclaimed here. */
        seL4_CPtr libc_l0 = alloc_object(seL4_RISCV_PageTableObject, 0);
        if (!libc_l0) return -ENOMEM;
        if (spawn_record_pt(libc_l0) != 0) return -ENOMEM;
        err = qsoe_riscv_pagetable_map(libc_l0, vspace, DL_LIBC_LOAD_VA,
                                        QSOE_VM_ATTR_DEFAULT);
        if (err) { tm_err("spawn: libc L0 PT map failed"); return -ENOMEM; }

        seL4_CPtr rtld_l0 = alloc_object(seL4_RISCV_PageTableObject, 0);
        if (!rtld_l0) return -ENOMEM;
        if (spawn_record_pt(rtld_l0) != 0) return -ENOMEM;
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
        tm_dbg("spawn: libc.so relocs %lu/%lu (%lu skipped)", ap, tot, sk);

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
        tm_dbg("spawn: rtld relocs %lu/%lu (%lu skipped)", ap, tot, sk);

        if (tm_reloc_apply(&main_view, /*bias=*/0, &libc_resolver,
                            reloc_write_cb, tm_reloc_skip_warn,
                            (void *)"main",
                            &ap, &tot, &sk) != 0) {
            tm_err("spawn: main reloc failed");
            return -ENOEXEC;
        }
        tm_dbg("spawn: main relocs %lu/%lu (%lu skipped)", ap, tot, sk);

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
        tm_dbg("spawn: skip rtld magic=%02x%02x%02x%02x rtld_entry=%08lx pc=%08lx phdr_va=%08lx",
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
    if (spawn_record_frame(CHILD_IPC_BUFFER, ipc_frame) != 0) return -ENOMEM;

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
    if (spawn_record_frame(CHILD_TCB_BASE, tcb_frame) != 0) return -ENOMEM;

    /* 4c. Sysmap page -- one READ-ONLY 'PSYS' page at QSOE_SYSMAP_VA
     *     carrying the platform catalog (mtime freq, cpu count, PCI
     *     ECAM + MMIO window).  The shared libc hwi_init() reads it
     *     with no IPC, exactly as on NQ where the Skimmer kernel maps
     *     it in the boot PT.  Built once at boot by tm_sysmap_build();
     *     each child gets its own copy mapped read-only.  Absent only if
     *     the FDT carried no usable platform info -- then hwi_init falls
     *     back to its built-in defaults, as before. */
    {
        const void *smp = 0;
        if (tm_sysmap_get(&smp, 0) == 0 && smp) {
            seL4_CPtr smap_frame = alloc_object(seL4_RISCV_4K_Page, 0);
            if (!smap_frame) return -ENOMEM;
            err = scratch_map(smap_frame);
            if (err) return -ENOMEM;
            qmemcpy((void *)TM_SCRATCH_VADDR, smp, 0x1000);
            err = scratch_unmap(smap_frame);
            if (err) return -ENOMEM;
            err = qsoe_riscv_page_map(smap_frame, vspace, CHILD_SYSMAP_BASE,
                                      QSOE_RIGHTS_RO, QSOE_VM_ATTR_DEFAULT);
            if (err) { tm_err("spawn: sysmap Page_Map failed"); return -ENOMEM; }
            if (spawn_record_frame(CHILD_SYSMAP_BASE, smap_frame) != 0)
                return -ENOMEM;
        }
    }

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
        if (spawn_record_frame(va, stack_frames[i]) != 0) return -ENOMEM;
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

    /* 5b'. Copy the child's own CNode cap into its slot
     *      QSOE_CAP_CNODE_SELF so the child can move a reply object
     *      within its own CSpace from inside — required for resmgr
     *      park-the-caller patterns (devc-ser8250 RX). */
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

    /* (No spawn-time UART cap granting.)  devc-ser8250 maps the 16550
     * itself via mmap(MAP_PHYS, UART_PHYS) and claims PLIC line 10 at
     * runtime via InterruptAttachThread.  The old v0.6.1 ELF-name-gated
     * block that pre-mapped the UART MMIO + pre-minted the IRQHandler
     * was removed (v0.10): besides being dead, its retype of a 4 KiB
     * frame from the UART device-untyped advanced that untyped's
     * watermark, so devc's own mmap_phys then landed one frame past the
     * UART (phys 0x10001000, the virtio window) -- the driver mapped the
     * wrong device and uart_tx spun forever on a bogus LSR. */

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
     * priority) via SetSchedParams.  Spawned processes run in the QNX
     * default user band (TM_PRIO_USER_DEFAULT), well below taskman; taskman
     * blocks on Recv when idle, so user threads always get the CPU. */
    seL4_CPtr sc = tm_sched_context_create(/*core=*/0);
    if (!sc) { tm_err("spawn: sched-context create failed"); return -ENOMEM; }
    /* Graceful crash: give the main thread a fault handler -- a badged
     * Send+GrantReply cap to taskman's primary EP (QSOE_RIGHTS_SEND
     * already grants reply).  On a fatal U-mode fault seL4 delivers a
     * fault IPC here (badge = pid | TM_FAULT_BADGE_FLAG) instead of
     * wedging the thread; the dispatcher then terminates the process.
     * The TCB derives its own copy of the cap, so our temp slot is
     * reclaimed right after. */
    seL4_CPtr fault_ep = taskman_alloc_empty_slot();
    err = qsoe_cnode_mint(s_cnode_root, fault_ep, TM_DEPTH_TASKMAN,
                          s_cnode_root, primary_ep, TM_DEPTH_TASKMAN,
                          QSOE_RIGHTS_SEND,
                          TM_FAULT_BADGE_FLAG | (seL4_Word)pid);
    if (err) { tm_err("spawn: fault-ep mint failed"); return -ENOMEM; }
    err = qsoe_tcb_set_sched_params(tcb, seL4_CapInitThreadTCB,
                                    /*mcp=*/TM_PRIO_USER_DEFAULT,
                                    /*prio=*/TM_PRIO_USER_DEFAULT,
                                    sc, fault_ep);
    if (err) { tm_err("spawn: TCB_SetSchedParams failed"); return -ENOMEM; }
    /* The minted fault cap must stay in our CSpace -- the TCB references
     * it (deleting it strips the handler).  Stashed in the record below
     * and freed in teardown once the TCB is gone. */

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
    /* Record the child's untyped budget master for cleanup on terminate,
     * and capture the ELF basename as the process name for /proc. */
    tm_process_t *prec = tm_process_lookup(pid);
    if (prec) {
        prec->untyped_budget = child_untyped;
        prec->fault_ep       = fault_ep;
        /* Seed the main thread's tracked scheduling state to match the
         * priority/policy it was just configured with (SetSchedParams
         * above), so SchedGet reports the truth before any SchedSet. */
        prec->sched_prio     = TM_PRIO_USER_DEFAULT;
        prec->sched_policy   = TM_SCHED_RR;
        const char *base = elf_name ? elf_name : "?";
        for (const char *s = base; *s; ++s)
            if (*s == '/') base = s + 1;
        unsigned ni = 0;
        while (base[ni] != '\0' && ni < sizeof prec->name - 1) {
            prec->name[ni] = base[ni];
            ++ni;
        }
        prec->name[ni] = '\0';
        /* Main-thread ps(1) label starts empty (the main thread tags
         * itself "main" via ThreadCtl at startup); clear it here so a
         * reused process slot never shows a stale label. */
        prec->main_name[0] = '\0';
    }
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
    tm_dbg("spawn: probe channel pid=%lu chid=1 -> idx=%ld",
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

    /* Slot reclamation: move the image-frame caps (the bulk of this
     * process's cap count) out of the flat root CNode into its own
     * object CNode.  Their root slots return to the free list right
     * away; on exit the objcnode -- a pp_ut child -- is destroyed by
     * Revoke(pput), freeing all of them together.  The page mapping
     * lives in the frame cap, so it survives the move; safe now that the
     * loads and the relocation pass have finished touching the frames. */
    {
        tm_process_t *op = tm_process_lookup(pid);
        if (op) {
            op->sc = sc;   /* main-thread SC slot, freed in teardown */
            seL4_CPtr objc = alloc_object(seL4_CapTableObject, TM_OBJCNODE_RADIX);
            if (!objc) { tm_err("spawn: objcnode alloc failed"); return -ENOMEM; }
            op->objcnode      = objc;
            op->objcnode_next = 0;
            for (int i = 0; i < s_frame_count; ++i) {
                seL4_CPtr src  = s_frames[i].frame;
                /* RELRO pages keep their cap INVOKEABLE (left in the root
                 * slot, recorded in op->mprot[]) so TM_REQ_MPROTECT can
                 * re-map them; everything else moves to the objcnode. */
                if (va_in_relro(s_frames[i].va_page)) {
                    if (op->mprot_count >= TM_MAX_MPROT) {
                        tm_err("spawn: pid %ld RELRO tracker full (cap=%d)",
                               (long)pid, TM_MAX_MPROT);
                        return -ENOMEM;
                    }
                    op->mprot[op->mprot_count].va_page = s_frames[i].va_page;
                    op->mprot[op->mprot_count].frame   = src;
                    op->mprot_count++;
                    continue;
                }
                seL4_Word merr = qsoe_cnode_move(objc,
                                                 (seL4_Word)op->objcnode_next,
                                                 TM_OBJCNODE_RADIX,
                                                 s_cnode_root, src,
                                                 TM_DEPTH_TASKMAN);
                if (merr) {
                    tm_err("spawn: objcnode move failed err=%lu",
                           (unsigned long)merr);
                    return -ENOMEM;
                }
                op->objcnode_va[op->objcnode_next] = s_frames[i].va_page;
                op->objcnode_next++;
                taskman_free_slot(src);
            }
            /* Same for the per-spawn page-table caps: relocate into the
             * objcnode (the mapping lives in the parent PT, not the cap,
             * so the move is transparent) and reclaim their root slots. */
            for (int i = 0; i < s_pt_count; ++i) {
                seL4_CPtr src  = s_pt_slots[i];
                seL4_Word merr = qsoe_cnode_move(objc,
                                                 (seL4_Word)op->objcnode_next,
                                                 TM_OBJCNODE_RADIX,
                                                 s_cnode_root, src,
                                                 TM_DEPTH_TASKMAN);
                if (merr) {
                    tm_err("spawn: objcnode PT move failed err=%lu",
                           (unsigned long)merr);
                    return -ENOMEM;
                }
                op->objcnode_va[op->objcnode_next] = 0;
                op->objcnode_next++;
                taskman_free_slot(src);
            }
        }
    }

    /* 9. Liftoff. */
    err = qsoe_tcb_resume(tcb);
    if (err) { tm_err("spawn: TCB_Resume failed"); return -ENOMEM; }

    return 0;
}
