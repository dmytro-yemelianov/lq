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
#include "../sel4_syscalls.h"
#include "../sel4_types.h"
#include "../qsoe_invoke.h"

static seL4_BootInfo *s_bi;

void tm_mem_set_bootinfo(seL4_BootInfo *bi)
{
    s_bi = bi;
}

/* Find a device-untyped that exactly bases at `paddr` and is at
 * least `len` bytes long.  Returns the cap slot (`bi->untyped.start
 * + idx`) and sets *out_sizebits to the UT's sizeBits, or 0 / -1
 * on miss.  For v0.8 we require an exact base match — relaxing this
 * (mapping sub-regions of large UTs at offset) needs an offset-
 * tracking allocator we'll add when something actually wants it. */
static seL4_CPtr find_device_ut_exact(unsigned long paddr,
                                       unsigned long len,
                                       unsigned *out_sizebits)
{
    if (!s_bi) return 0;
    unsigned n = s_bi->untyped.end - s_bi->untyped.start;
    for (unsigned i = 0; i < n; ++i) {
        if (!s_bi->untypedList[i].isDevice) continue;
        unsigned long base = s_bi->untypedList[i].paddr;
        unsigned long size = 1UL << s_bi->untypedList[i].sizeBits;
        if (base == paddr && size >= len) {
            if (out_sizebits) *out_sizebits = s_bi->untypedList[i].sizeBits;
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
            sel4_debug_puts("tm_mmap_serve: Mega_Page alloc failed\n");
            return -ENOMEM;
        }
        seL4_Word err = qsoe_riscv_page_map(frame, proc->vspace,
                                            base + i * QSOE_MEGA_PAGE,
                                            QSOE_RIGHTS_ALL,
                                            QSOE_VM_ATTR_DEFAULT);
        if (err) {
            sel4_debug_puts("tm_mmap_serve: Page_Map failed\n");
            return -ENOMEM;
        }
    }

    proc->mmap_top = base + bytes;
    *out_vaddr = base;
    return 0;
}

/* MAP_PHYS — retype 4 KiB pages from a device-untyped, map them
 * into the client's VSpace, return the base VA.                   */
static int mmap_phys(tm_process_t *proc, unsigned long phys,
                     unsigned long len, unsigned long *out_vaddr)
{
    unsigned ut_sizebits = 0;
    seL4_CPtr ut = find_device_ut_exact(phys, len, &ut_sizebits);
    if (!ut) {
        sel4_debug_puts("tm_mmap_serve(PHYS): no matching device-UT\n");
        return -ENODEV;
    }

    /* Round len up to 4 KiB; cap at UT size. */
    unsigned long pages_4k = (len + QSOE_PAGE_4K - 1) / QSOE_PAGE_4K;
    unsigned long ut_size = 1UL << ut_sizebits;
    if (pages_4k * QSOE_PAGE_4K > ut_size) {
        return -EINVAL;
    }

    /* Pick a fresh VA range — align the base to 2 MiB (a Mega_Page
     * boundary) so the L0 PTs we allocate cover it cleanly. */
    unsigned long base_va = (proc->mmap_top + QSOE_MEGA_PAGE - 1) &
                            ~(QSOE_MEGA_PAGE - 1);
    unsigned long end_va  = base_va + pages_4k * QSOE_PAGE_4K;

    /* Allocate one L0 PageTable per 2 MiB span touched.  Mapping an
     * L0 PT that's already mapped under our process's L1 returns
     * seL4_DeleteFirst which we'd need to handle if the same region
     * were touched again — for now mmap_top monotonic guarantees
     * we won't.  */
    for (unsigned long va = base_va & ~(QSOE_MEGA_PAGE - 1);
         va < end_va; va += QSOE_MEGA_PAGE) {
        seL4_CPtr l0 = taskman_alloc_and_retype(seL4_RISCV_PageTableObject, 0);
        if (!l0) return -ENOMEM;
        seL4_Word err = qsoe_riscv_pagetable_map(l0, proc->vspace, va,
                                                  QSOE_VM_ATTR_DEFAULT);
        if (err) {
            sel4_debug_puts("tm_mmap_serve(PHYS): L0 PageTable_Map failed\n");
            return -ENOMEM;
        }
    }

    /* Retype N consecutive 4 KiB Pages from the device-untyped and
     * map them into the client's VSpace.  Each retype advances the
     * kernel's per-UT watermark; one device-UT can be carved up
     * across multiple MAP_PHYS calls.  Allocate caps in taskman's
     * CSpace so we can invoke Page_Map; the kernel maps the device
     * frame into the caller's VSpace, no copy needed in the child. */
    for (unsigned long i = 0; i < pages_4k; ++i) {
        seL4_CPtr page = taskman_alloc_empty_slot();
        if (!page) return -ENOMEM;
        seL4_Word err = qsoe_untyped_retype(ut, seL4_RISCV_4K_Page, 0,
                                             s_cnode_root, 0, 0, page, 1);
        if (err) {
            sel4_debug_puts("tm_mmap_serve(PHYS): device retype failed\n");
            return -ENOMEM;
        }
        err = qsoe_riscv_page_map(page, proc->vspace,
                                   base_va + i * QSOE_PAGE_4K,
                                   QSOE_RIGHTS_ALL,
                                   QSOE_VM_ATTR_DEFAULT);
        if (err) {
            sel4_debug_puts("tm_mmap_serve(PHYS): Page_Map failed\n");
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
