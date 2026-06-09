/*
 * mem/mmap.c — Memory Manager.
 *
 * Two flavours behind one entry point (tm_mmap_serve):
 *
 *   ANONYMOUS (default).  Retypes RAM-backed Mega_Pages (2 MiB) out
 *   of taskman's untyped pool, maps them contiguously into the
 *   caller's VSpace at its mmap_top cursor.  Granularity 2 MiB —
 *   sub-Mega_Page requests round up.  This is what malloc.c's
 *   page-pool grabber asks for.
 *
 *   MAP_PHYS (v0.8+).  Caller names a physical address; we walk the
 *   bootinfo's device-untyped list, find the matching region, and
 *   retype 4 KiB Pages out of it into the caller's VSpace.  Used by
 *   userland drivers / resmgrs (pci-server's ECAM window, future
 *   devb-nvme's MMIO).  For v0.8 the requested phys must equal the
 *   device-UT base — sub-region offsets need offset-retype logic
 *   we'll add when a caller actually needs it.
 *
 * Carried forward from v0.7: bump-allocated VAs (mmap_top), no
 * munmap.  All process state lives in tm_process_t.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mem.h"
#include "../proc/proc.h"
#include "../sel4_types.h"
#include "../qsoe_invoke.h"
#include "../tm_log.h"

static seL4_BootInfo *s_bi;

void tm_mem_set_bootinfo(seL4_BootInfo *bi)
{
    s_bi = bi;
}

/* Find a device-untyped that CONTAINS [paddr, paddr+len).  Returns
 * the cap slot, sets *out_sizebits to the UT's sizeBits, and
 * *out_offset to (paddr - ut_base).  Returns 0 on miss.            */
static seL4_CPtr find_device_ut_containing(unsigned long paddr,
                                            unsigned long len,
                                            unsigned *out_sizebits,
                                            unsigned long *out_offset)
{
    if (!s_bi) return 0;
    unsigned n = s_bi->untyped.end - s_bi->untyped.start;
    for (unsigned i = 0; i < n; ++i) {
        if (!s_bi->untypedList[i].isDevice) continue;
        unsigned long base = s_bi->untypedList[i].paddr;
        unsigned long size = 1UL << s_bi->untypedList[i].sizeBits;
        if (paddr >= base && (paddr + len) <= (base + size)) {
            if (out_sizebits) *out_sizebits = s_bi->untypedList[i].sizeBits;
            if (out_offset)   *out_offset   = paddr - base;
            return s_bi->untyped.start + i;
        }
    }
    return 0;
}



/* Anonymous (Mega_Page) mmap — the v0.7 path, untouched.   */
static int mmap_anonymous(tm_process_t *proc, unsigned long len,
                          unsigned long *out_vaddr)
{
    /* Round up to a multiple of QSOE_MEGA_PAGE. */
    unsigned long bytes = (len + QSOE_MEGA_PAGE - 1) & ~(QSOE_MEGA_PAGE - 1);
    unsigned long base  = proc->mmap_top;
    unsigned long pages = bytes / QSOE_MEGA_PAGE;

    for (unsigned long i = 0; i < pages; ++i) {
        seL4_CPtr frame = taskman_alloc_and_retype(seL4_RISCV_Mega_Page, 0);
        if (!frame) {
            tm_err("tm_mmap_serve: Mega_Page alloc failed");
            return -ENOMEM;
        }
        unsigned long va = base + i * QSOE_MEGA_PAGE;
        seL4_Word err = qsoe_riscv_page_map(frame, proc->vspace, va,
                                            QSOE_RIGHTS_ALL,
                                            QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("tm_mmap_serve: Page_Map failed");
            return -ENOMEM;
        }
        /* Per-process (va_page, frame_cap) tracker -- lets later
         * TM_REQ_* handlers (notably TM_REQ_SPAWN's args-page
         * read-back) find the frame backing this VA without having
         * to walk the child's page tables.  Loud crash on overflow
         * per the no-silent-truncation rule. */
        if (proc->mmap_count >= TM_MAX_MMAP_PER_PROC) {
            tm_err("tm_mmap_serve: pid %d mmap tracker full (cap=%d) -- "
                   "bump TM_MAX_MMAP_PER_PROC in <tm_limits.h>",
                   (long)proc->pid, TM_MAX_MMAP_PER_PROC);
            return -ENOMEM;
        }
        proc->mmap[proc->mmap_count].va_page = va;
        proc->mmap[proc->mmap_count].frame   = frame;
        proc->mmap_count++;
    }

    proc->mmap_top = base + bytes;
    *out_vaddr = base;
    return 0;
}

/* Count trailing zero bits of x (x must be non-zero).             */
static unsigned ctz_ul(unsigned long x)
{
    unsigned n = 0;
    while ((x & 1) == 0) { x >>= 1; ++n; }
    return n;
}

/* MAP_PHYS — map a device's MMIO into the caller's VSpace at 4 KiB
 * granularity.  Find the device-UT containing `phys`, advance its
 * watermark to the sub-region offset (by burning intermediate
 * retypes), then retype 4 KiB device Pages and map them as L0 leaves.
 * Returns the base VA.
 *
 * Why 4 KiB and not Mega_Pages: device MMIO regions are page-sized,
 * and a 2 MiB device superpage store-access-faults on QEMU virt (the
 * first store to the mapped UART faulted with RISC-V scause 7).  So
 * each 2 MiB span of the chosen VA range gets a fresh L0 page table
 * hung under the child's (already installed) L1, and the device frames
 * map as 4 KiB L0 leaves. */
static int mmap_phys(tm_process_t *proc, unsigned long phys,
                     unsigned long len, unsigned long *out_vaddr)
{
    unsigned ut_sizebits = 0;
    unsigned long ut_offset = 0;
    seL4_CPtr ut = find_device_ut_containing(phys, len, &ut_sizebits,
                                              &ut_offset);
    if (!ut) {
        tm_err("tm_mmap_serve(PHYS): no matching device-UT");
        return -ENODEV;
    }

    /* Require 4 KiB-page alignment for base; round length up to pages. */
    if (phys & (QSOE_PAGE_4K - 1)) {
        tm_err("tm_mmap_serve(PHYS): phys not page-aligned");
        return -EINVAL;
    }
    len = (len + QSOE_PAGE_4K - 1) & ~(QSOE_PAGE_4K - 1);
    unsigned long npages  = len / QSOE_PAGE_4K;
    unsigned long ut_size = 1UL << ut_sizebits;
    if (ut_offset + len > ut_size) return -EINVAL;

    /* Advance the kernel's watermark past `ut_offset` by retyping
     * power-of-2 chunks (as throw-away child UTs) until we land at
     * exactly `ut_offset`.  Greedy biggest-aligned-chunk first; since
     * `phys` and the UT base are page-aligned, chunks are >= 4 KiB.    */
    unsigned long advanced = 0;
    while (advanced < ut_offset) {
        unsigned long rem = ut_offset - advanced;
        unsigned chunk_sb = ctz_ul(rem);
        unsigned align_sb = ctz_ul(advanced ? advanced : ut_size);
        if (chunk_sb > align_sb) chunk_sb = align_sb;
        seL4_CPtr dummy = taskman_alloc_empty_slot();
        if (!dummy) return -ENOMEM;
        seL4_Word err = qsoe_untyped_retype(ut, seL4_UntypedObject,
                                             chunk_sb, s_cnode_root,
                                             0, 0, dummy, 1);
        if (err) {
            tm_err("tm_mmap_serve(PHYS): skip-retype failed");
            return -ENOMEM;
        }
        advanced += 1UL << chunk_sb;
    }

    /* Pick a fresh Mega-aligned VA base so each 2 MiB span lines up
     * with an L1 slot we can hang an L0 PT under. */
    unsigned long base_va = (proc->mmap_top + QSOE_MEGA_PAGE - 1) &
                            ~(QSOE_MEGA_PAGE - 1);

    for (unsigned long i = 0; i < npages; ++i) {
        unsigned long va = base_va + i * QSOE_PAGE_4K;

        /* At each 2 MiB boundary, hang a fresh RAM-backed L0 PT (from
         * taskman's own untyped pool) under the child's already-present
         * L1, so the 4 KiB device frames below have a leaf level. */
        if ((va & (QSOE_MEGA_PAGE - 1)) == 0) {
            seL4_CPtr l0 = taskman_alloc_and_retype(
                               seL4_RISCV_PageTableObject, 0);
            if (!l0) {
                tm_err("tm_mmap_serve(PHYS): L0 PT alloc failed");
                return -ENOMEM;
            }
            seL4_Word e = qsoe_riscv_pagetable_map(l0, proc->vspace, va,
                                                   QSOE_VM_ATTR_DEFAULT);
            if (e) {
                tm_err("tm_mmap_serve(PHYS): L0 PageTable_Map failed");
                return -ENOMEM;
            }
        }

        /* Retype the next 4 KiB device frame from the device-UT and map
         * it as an L0 leaf at `va`. */
        seL4_CPtr page = taskman_alloc_empty_slot();
        if (!page) return -ENOMEM;
        seL4_Word err = qsoe_untyped_retype(ut, seL4_RISCV_4K_Page, 0,
                                             s_cnode_root, 0, 0, page, 1);
        if (err) {
            tm_err("tm_mmap_serve(PHYS): 4K device retype failed");
            return -ENOMEM;
        }
        err = qsoe_riscv_page_map(page, proc->vspace, va,
                                   QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("tm_mmap_serve(PHYS): 4K device Page_Map failed");
            return -ENOMEM;
        }
    }

    proc->mmap_top = base_va + len;
    *out_vaddr = base_va;
    return 0;
}

int tm_mmap_serve(pid_t caller, unsigned long len, unsigned long flags,
                  unsigned long phys, unsigned long *out_vaddr)
{
    tm_process_t *proc = tm_process_lookup(caller);
    if (!proc) return -ESRCH;
    if (len == 0) return -EINVAL;

    if (flags & TM_MMAP_FLAG_PHYS) {
        return mmap_phys(proc, phys, len, out_vaddr);
    }
    return mmap_anonymous(proc, len, out_vaddr);
}

/* Walk proc->mmap[] for an entry whose va_page == va.  Returns index
 * or -1.  Linear scan: tracker is bounded by TM_MAX_MMAP_PER_PROC. */
static int find_mmap_idx(tm_process_t *proc, unsigned long va)
{
    for (int i = 0; i < proc->mmap_count; ++i) {
        if (proc->mmap[i].va_page == va) return i;
    }
    return -1;
}

int tm_munmap_serve(pid_t caller, unsigned long vaddr, unsigned long len)
{
    tm_process_t *proc = tm_process_lookup(caller);
    if (!proc) return -ESRCH;
    if (len == 0) return -EINVAL;
    if (vaddr & (QSOE_MEGA_PAGE - 1)) return -EINVAL;

    /* Round up to Mega_Page; matches what mmap rounded up at alloc. */
    unsigned long bytes = (len + QSOE_MEGA_PAGE - 1) & ~(QSOE_MEGA_PAGE - 1);
    unsigned long pages = bytes / QSOE_MEGA_PAGE;

    /* First sweep: every page must be tracked, else bail BEFORE
     * touching anything.  Keeps caller bugs from half-unmapping a
     * range and leaving the tracker in a weird state. */
    for (unsigned long i = 0; i < pages; ++i) {
        if (find_mmap_idx(proc, vaddr + i * QSOE_MEGA_PAGE) < 0) {
            tm_err("tm_munmap_serve: pid %d va 0x%lx not in tracker",
                   (long)proc->pid, vaddr + i * QSOE_MEGA_PAGE);
            return -EINVAL;
        }
    }

    /* Second sweep: actually tear down.  Page_Unmap → CNode_Delete →
     * recycle slot → drop tracker entry by swap-with-last. */
    for (unsigned long i = 0; i < pages; ++i) {
        unsigned long va  = vaddr + i * QSOE_MEGA_PAGE;
        int           idx = find_mmap_idx(proc, va);
        seL4_CPtr     frm = proc->mmap[idx].frame;

        (void) qsoe_riscv_page_unmap(frm);
        (void) qsoe_cnode_delete(s_cnode_root, frm, TM_DEPTH_TASKMAN);
        taskman_free_slot(frm);

        int last = proc->mmap_count - 1;
        if (idx != last) proc->mmap[idx] = proc->mmap[last];
        proc->mmap_count = last;
    }
    return 0;
}
