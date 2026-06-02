/*
 * sys/initrd.c -- vestigial.  Excluded from the default link.
 *
 * Activate by adding -DTM_USE_INITRD_LOADER to TM_CFLAGS and adding
 * $(OBJDIR)/sys/initrd.o to TASKMAN_OBJS in taskman/Makefile.
 * The default build path embeds modpkg.cpio via .incbin instead, see
 * taskman/Makefile and main.c's #else branch.
 *
 * Why parked: seL4 zero-fills frames on retype-to-Frame for security,
 * which clobbers the kernel-placed initrd payload at retype time.
 * Reviving this path needs an elfloader change that hands the cpio
 * bytes through a reserved-memory + device-untyped path so the frames
 * containing the payload are NOT retyped from a RAM untyped.  Kept on
 * disk because that work is on the roadmap (and because we may want a
 * large external ramdisk later, exceeding what .incbin is sensible
 * for).
 *
 * ---------- Original design notes follow ----------
 *
 * Discover and map the userland CPIO archive that QEMU hands taskman
 * via `-initrd`.
 *
 * Mirrors the NQ initrd path (project_initrd_boot memory) but the
 * seL4 plumbing is different.  Skimmer reads the FDT in kernel mode
 * and reserves the initrd region; seL4 does NOT.  So on LQ taskman:
 *
 *   1. Reads /chosen/linux,initrd-{start,end} from the FDT that seL4
 *      publishes in bootinfo extras.
 *   2. Walks bi->untypedList for the RAM untyped that contains the
 *      initrd PA range.
 *   3. Burns the kernel-side watermark of that untyped to land at
 *      exactly the initrd offset (chunk-by-chunk power-of-2
 *      throwaway retypes — same idiom as mem/mmap.c's MAP_PHYS path).
 *   4. Retypes 4 KiB frames out of the UT at successive offsets and
 *      maps them at TM_INITRD_VA in taskman's own VSpace.  L1 + L0
 *      page-tables for the chosen VA range are allocated up-front
 *      (same pattern as proc/spawn.c's ensure_scratch_pt).
 *
 * Result: taskman holds a VA pointer + length into the cpio archive,
 * which feeds tm_set_userland_cpio / tm_cpiofs_set_cpio / cpio_get_file
 * exactly the way the retired `.incbin` _userland_cpio_start did.  No
 * compile-time embedding.
 *
 * Replaces the userland_archive.S/.incbin shim (retired 2026-05-31).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <r_tty@yahoo.co.uk>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>            /* uint{32,64}_t in the libqsoe style */

#include "../sel4_types.h"
#include "../qsoe_invoke.h"
#include "../proc/proc.h"          /* s_untyped, s_cnode_root, taskman_alloc_empty_slot */
#include "../tm_log.h"
#include "initrd.h"

extern int  tm_fdt_path(const void *blob, const char *path);
extern int  tm_fdt_prop_u64(const void *blob, int node,
                            const char *name, uint64_t *out);

/* TM_INITRD_VA -- a 1 GiB-aligned VA slice for the mapped archive.
 * Taskman.elf itself lives at vaddr 0x10000..0x6ffff (per the
 * elfloader log) so 0x80000000 is comfortably out of the way; same
 * SV39 entry-2 slice that nothing else in taskman touches.  Up to
 * 2 MiB of initrd fits under a single L0 PT; larger archives would
 * need more L0 PTs allocated. */
#define TM_INITRD_VA          0x80000000UL
#define TM_INITRD_MAX_BYTES   (2UL * 1024 * 1024)
#define PAGE_BITS             12
#define PAGE_SIZE             (1UL << PAGE_BITS)
#define PAGE_MASK             (PAGE_SIZE - 1)

/* Find a RAM (non-device) untyped that fully contains [paddr,paddr+len).
 * Returns the cap slot, sets *out_sizebits and *out_offset.  0 on miss. */
static seL4_CPtr find_ram_ut_containing(seL4_BootInfo *bi,
                                         unsigned long paddr,
                                         unsigned long len,
                                         unsigned *out_sizebits,
                                         unsigned long *out_offset)
{
    unsigned n = bi->untyped.end - bi->untyped.start;
    for (unsigned i = 0; i < n; ++i) {
        if (bi->untypedList[i].isDevice) continue;
        unsigned long base = bi->untypedList[i].paddr;
        unsigned long size = 1UL << bi->untypedList[i].sizeBits;
        if (paddr >= base && (paddr + len) <= (base + size)) {
            *out_sizebits = bi->untypedList[i].sizeBits;
            *out_offset   = paddr - base;
            return bi->untyped.start + i;
        }
    }
    return 0;
}

static unsigned ctz_ul(unsigned long x)
{
    unsigned n = 0;
    while ((x & 1) == 0) { x >>= 1; ++n; }
    return n;
}

/* Burn the kernel-side watermark of `ut` (size 1<<ut_sizebits) by
 * retyping power-of-2 throwaway sub-untypeds until we land at exactly
 * `target_offset`.  Greedy biggest-chunk-first, constrained by the
 * alignment of (current_advance, remaining).  Returns 0 / -1. */
static int advance_ut_watermark(seL4_CPtr ut,
                                 unsigned long target_offset,
                                 unsigned ut_sizebits)
{
    unsigned long ut_size = 1UL << ut_sizebits;
    unsigned long advanced = 0;
    while (advanced < target_offset) {
        unsigned long rem = target_offset - advanced;
        unsigned chunk_sb = ctz_ul(rem);
        unsigned align_sb = ctz_ul(advanced ? advanced : ut_size);
        if (chunk_sb > align_sb) chunk_sb = align_sb;
        seL4_CPtr dummy = taskman_alloc_empty_slot();
        if (!dummy) return -1;
        seL4_Word err = qsoe_untyped_retype(ut, seL4_UntypedObject,
                                             chunk_sb, s_cnode_root,
                                             0, 0, dummy, 1);
        if (err) {
            tm_err("initrd: skip-retype failed err=%lu",
                   (unsigned long) err);
            return -1;
        }
        advanced += 1UL << chunk_sb;
    }
    return 0;
}

/* Allocate one PageTable object from the largest RAM UT (s_untyped). */
static seL4_CPtr alloc_pt_from_s_untyped(void)
{
    seL4_CPtr slot = taskman_alloc_empty_slot();
    if (!slot) return 0;
    seL4_Word err = qsoe_untyped_retype(s_untyped,
                                         seL4_RISCV_PageTableObject,
                                         0, s_cnode_root, 0, 0, slot, 1);
    return (err == 0) ? slot : 0;
}

/* Install L1 + L0 page-tables under taskman's vspace so that 4 KiB
 * page mappings at TM_INITRD_VA..+TM_INITRD_MAX_BYTES will succeed. */
static int ensure_initrd_pts(void)
{
    seL4_CPtr l1 = alloc_pt_from_s_untyped();
    if (!l1) { tm_err("initrd: L1 PT alloc failed"); return -1; }
    seL4_Word err = qsoe_riscv_pagetable_map(l1, seL4_CapInitThreadVSpace,
                                              TM_INITRD_VA,
                                              QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("initrd: L1 PT map failed err=%lu", (unsigned long) err);
        return -1;
    }
    seL4_CPtr l0 = alloc_pt_from_s_untyped();
    if (!l0) { tm_err("initrd: L0 PT alloc failed"); return -1; }
    err = qsoe_riscv_pagetable_map(l0, seL4_CapInitThreadVSpace,
                                    TM_INITRD_VA,
                                    QSOE_VM_ATTR_DEFAULT);
    if (err) {
        tm_err("initrd: L0 PT map failed err=%lu", (unsigned long) err);
        return -1;
    }
    return 0;
}

int tm_initrd_load(seL4_BootInfo *bi, const void *fdt,
                   const void **out_data, unsigned long *out_size)
{
    *out_data = 0;
    *out_size = 0;

    if (!fdt) {
        tm_err("initrd: no FDT in bootinfo");
        return -1;
    }

    int chosen = tm_fdt_path(fdt, "/chosen");
    if (chosen < 0) {
        tm_err("initrd: /chosen not in FDT");
        return -1;
    }

    uint64_t initrd_start = 0, initrd_end = 0;
    if (tm_fdt_prop_u64(fdt, chosen, "linux,initrd-start",
                         &initrd_start) != 0 ||
        tm_fdt_prop_u64(fdt, chosen, "linux,initrd-end",
                         &initrd_end) != 0) {
        tm_err("initrd: /chosen/linux,initrd-* missing -- "
               "did emu.sh forget `-initrd`?");
        return -1;
    }

    if (initrd_end <= initrd_start) {
        tm_err("initrd: empty range start=%lx end=%lx",
               (unsigned long) initrd_start, (unsigned long) initrd_end);
        return -1;
    }

    unsigned long initrd_pa   = (unsigned long) initrd_start;
    unsigned long initrd_size = (unsigned long) (initrd_end - initrd_start);

    if (initrd_size > TM_INITRD_MAX_BYTES) {
        tm_err("initrd: %lu bytes exceeds map window %lu",
               initrd_size, TM_INITRD_MAX_BYTES);
        return -1;
    }

    tm_info("initrd: PA=0x%lx size=%lu", initrd_pa, initrd_size);

    /* Align to 4 KiB for retype.  PA-aligned-down + length includes
     * the byte-into-page that the initrd actually starts at. */
    unsigned long page_pa     = initrd_pa & ~PAGE_MASK;
    unsigned long byte_in_pg  = initrd_pa & PAGE_MASK;
    unsigned long pages       = (byte_in_pg + initrd_size + PAGE_SIZE - 1)
                                >> PAGE_BITS;

    unsigned ut_sizebits = 0;
    unsigned long ut_offset = 0;
    seL4_CPtr ut = find_ram_ut_containing(bi, page_pa,
                                           pages << PAGE_BITS,
                                           &ut_sizebits, &ut_offset);
    if (!ut) {
        tm_err("initrd: no RAM UT contains [0x%lx..0x%lx)",
               page_pa, page_pa + (pages << PAGE_BITS));
        return -1;
    }

    if (advance_ut_watermark(ut, ut_offset, ut_sizebits) != 0)
        return -1;

    if (ensure_initrd_pts() != 0) return -1;

    /* Retype 4 KiB frames at successive offsets, map them. */
    for (unsigned long i = 0; i < pages; ++i) {
        seL4_CPtr frame = taskman_alloc_empty_slot();
        if (!frame) {
            tm_err("initrd: slot alloc failed at page %lu", i);
            return -1;
        }
        seL4_Word err = qsoe_untyped_retype(ut, seL4_RISCV_4K_Page,
                                             0, s_cnode_root,
                                             0, 0, frame, 1);
        if (err) {
            tm_err("initrd: 4K_Page retype failed at page %lu err=%lu",
                   i, (unsigned long) err);
            return -1;
        }
        err = qsoe_riscv_page_map(frame, seL4_CapInitThreadVSpace,
                                   TM_INITRD_VA + (i << PAGE_BITS),
                                   QSOE_RIGHTS_ALL,
                                   QSOE_VM_ATTR_DEFAULT);
        if (err) {
            tm_err("initrd: Page_Map failed at page %lu err=%lu",
                   i, (unsigned long) err);
            return -1;
        }
    }

    *out_data = (const void *) (TM_INITRD_VA + byte_in_pg);
    *out_size = initrd_size;
    return 0;
}
