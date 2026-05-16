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
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
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
        seL4_Word err = qsoe_riscv_page_map(frame, proc->vspace,
                                            base + i * QSOE_MEGA_PAGE,
                                            QSOE_RIGHTS_ALL,
                                            QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("tm_mmap_serve: Page_Map failed");
            return -ENOMEM;
        }
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

/* MAP_PHYS — find the device-UT containing `phys`, advance its
 * watermark to the offset (by burning intermediate retypes), then
 * retype Mega_Page (2 MiB) frames for the actual mapping and slot
 * them into the client's VSpace.  Returns the base VA.
 *
 * v0.8-rc2: requires `phys` and `len` to be 2 MiB-aligned.  Smaller
 * 4 KiB mappings (e.g. single-page MMIO registers) land in rc3
 * when the first device-driver port needs them. */
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

    /* Require Mega_Page alignment for both base and length. */
    if (phys & (QSOE_MEGA_PAGE - 1)) {
        tm_err("tm_mmap_serve(PHYS): phys not Mega-aligned");
        return -EINVAL;
    }
    if (len & (QSOE_MEGA_PAGE - 1)) {
        /* Round up to next Mega_Page. */
        len = (len + QSOE_MEGA_PAGE - 1) & ~(QSOE_MEGA_PAGE - 1);
    }
    unsigned long pages_2m = len / QSOE_MEGA_PAGE;
    unsigned long ut_size  = 1UL << ut_sizebits;
    if (ut_offset + len > ut_size) return -EINVAL;

    /* Advance the kernel's watermark past `ut_offset` by retyping
     * power-of-2 chunks (as throw-away child UTs) until we land at
     * exactly `ut_offset`.  Greedy biggest-chunk-first.             */
    unsigned long advanced = 0;
    while (advanced < ut_offset) {
        unsigned long rem = ut_offset - advanced;
        /* Biggest chunk we can carve: limited by remaining offset AND
         * by alignment of (UT base + advanced). */
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

    /* Pick a fresh VA range — Mega-aligned.  No L0 PTs needed: each
     * Mega_Page is a L1-level leaf in SV39, and the L1 PTs covering
     * mmap_top are already installed at process startup (same as the
     * anonymous-mmap path). */
    unsigned long base_va = (proc->mmap_top + QSOE_MEGA_PAGE - 1) &
                            ~(QSOE_MEGA_PAGE - 1);
    unsigned long end_va  = base_va + len;

    /* Retype Mega_Pages from the device-UT and map them in. */
    for (unsigned long i = 0; i < pages_2m; ++i) {
        seL4_CPtr page = taskman_alloc_empty_slot();
        if (!page) return -ENOMEM;
        seL4_Word err = qsoe_untyped_retype(ut, seL4_RISCV_Mega_Page, 0,
                                             s_cnode_root, 0, 0, page, 1);
        if (err) {
            tm_err("tm_mmap_serve(PHYS): Mega_Page retype failed");
            return -ENOMEM;
        }
        err = qsoe_riscv_page_map(page, proc->vspace,
                                   base_va + i * QSOE_MEGA_PAGE,
                                   QSOE_RIGHTS_ALL,
                                   QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("tm_mmap_serve(PHYS): Mega_Page_Map failed");
            return -ENOMEM;
        }
    }

    proc->mmap_top = end_va;
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
