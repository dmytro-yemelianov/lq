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
#include <tm_log.h>

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
        /* Prefer a frame recycled by tm_munmap_serve over carving a
         * fresh one: deleting a Mega_Page cap never returns its 2 MiB
         * to the parent pp_ut untyped (only Revoke at exit does), so
         * without reuse a long-lived mmap/munmap churner leaks the
         * board.  A recycled frame keeps its old contents, so zero it
         * to honor MAP_ANONYMOUS. */
        seL4_CPtr frame;
        if (proc->mmap_free_count > 0) {
            frame = proc->mmap_free[--proc->mmap_free_count];
            if (tm_zero_megaframe(frame) != 0) {
                proc->mmap_free[proc->mmap_free_count++] = frame;  /* put back */
                tm_err("tm_mmap_serve: zeroing recycled Mega_Page failed");
                return -ENOMEM;
            }
        } else {
            frame = taskman_alloc_and_retype(seL4_RISCV_Mega_Page, 0);
        }
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
            tm_err("tm_mmap_serve: pid %ld mmap tracker full (cap=%d) -- "
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

/* ---- MAP_PHYS device-frame registry (v0.11) ----------------------
 * A device region is carved from its device-UT exactly ONCE.  A
 * device-UT's free index only ever advances (and the watermark advance
 * to reach a region's offset burns that space permanently), so a region
 * can never be re-carved -- a second consumer of the same PA (e.g.
 * sysinfo's ECAM walk after pci-server already mapped it) would fail.
 * Instead we keep the carved frame caps here, UNMAPPED, and hand every
 * requester -- including the first -- cnode_copy's of them mapped into
 * its own VSpace (seL4 lets one frame map into many VSpaces via copied
 * caps).  pci-server and sysinfo thus share the ECAM frames, and the
 * carve (incl. the throwaway advance) is paid once. */
#define TM_DEVMAP_MAX          16   /* distinct shared device regions */
#define TM_DEVMAP_MAX_FRAMES   64   /* frames per region */

typedef struct {
    int           in_use;
    unsigned long phys;        /* region base PA (granule-aligned) */
    unsigned long len;         /* region length, granule-rounded */
    unsigned long granule;     /* QSOE_MEGA_PAGE or QSOE_PAGE_4K */
    int           nframes;
    seL4_CPtr     frames[TM_DEVMAP_MAX_FRAMES];   /* carved, unmapped */
} tm_devmap_t;
static tm_devmap_t s_devmaps[TM_DEVMAP_MAX];

/* A registered region that fully covers [phys, phys+len). */
static tm_devmap_t *devmap_find(unsigned long phys, unsigned long len)
{
    for (int i = 0; i < TM_DEVMAP_MAX; ++i) {
        tm_devmap_t *d = &s_devmaps[i];
        if (d->in_use && d->phys <= phys && phys + len <= d->phys + d->len)
            return d;
    }
    return 0;
}

/* Carve [phys, phys+len) from its device-UT into a fresh registry entry.
 * Frames are retyped UNMAPPED; the watermark advance to the region's
 * offset is paid here, once.  Granule is 2 MiB when phys + the UT offset
 * are both 2 MiB-aligned (so a Mega_Page lands at the right PA) and the
 * region spans at least 2 MiB, else 4 KiB (e.g. the ser8250 UART, which
 * also store-faulted once as a 2 MiB superpage).  Returns the entry or 0
 * (logging the specific failure). */
static tm_devmap_t *devmap_carve(unsigned long phys, unsigned long len)
{
    unsigned ut_sizebits = 0;
    unsigned long ut_offset = 0;
    seL4_CPtr ut = find_device_ut_containing(phys, len, &ut_sizebits,
                                              &ut_offset);
    if (!ut) { tm_err("tm_mmap_serve(PHYS): no matching device-UT"); return 0; }

    unsigned long ut_size = 1UL << ut_sizebits;
    if (ut_offset + len > ut_size) {
        tm_err("tm_mmap_serve(PHYS): region exceeds device-UT");
        return 0;
    }

    unsigned long granule =
        ((phys & (QSOE_MEGA_PAGE - 1)) == 0 &&
         (ut_offset & (QSOE_MEGA_PAGE - 1)) == 0 &&
         len >= QSOE_MEGA_PAGE) ? QSOE_MEGA_PAGE : QSOE_PAGE_4K;
    unsigned long rlen = (len + granule - 1) & ~(granule - 1);
    int nframes = (int)(rlen / granule);
    if (nframes > TM_DEVMAP_MAX_FRAMES) {
        tm_err("tm_mmap_serve(PHYS): region too large (%d frames > %d)",
               nframes, TM_DEVMAP_MAX_FRAMES);
        return 0;
    }

    tm_devmap_t *d = 0;
    for (int i = 0; i < TM_DEVMAP_MAX; ++i)
        if (!s_devmaps[i].in_use) { d = &s_devmaps[i]; break; }
    if (!d) { tm_err("tm_mmap_serve(PHYS): device-map registry full"); return 0; }

    /* Advance the device-UT watermark to ut_offset with throwaway child
     * untypeds (greedy biggest-aligned chunk; phys + UT base are aligned). */
    unsigned long advanced = 0;
    while (advanced < ut_offset) {
        unsigned long rem = ut_offset - advanced;
        unsigned chunk_sb = ctz_ul(rem);
        unsigned align_sb = ctz_ul(advanced ? advanced : ut_size);
        if (chunk_sb > align_sb) chunk_sb = align_sb;
        seL4_CPtr dummy = taskman_alloc_empty_slot();
        if (!dummy) return 0;
        if (qsoe_untyped_retype(ut, seL4_UntypedObject, chunk_sb,
                                s_cnode_root, 0, 0, dummy, 1) != 0) {
            tm_err("tm_mmap_serve(PHYS): skip-retype failed");
            taskman_free_slot(dummy);
            return 0;
        }
        advanced += 1UL << chunk_sb;
    }

    /* Retype the device frames, UNMAPPED, into the registry. */
    seL4_Word type = (granule == QSOE_MEGA_PAGE) ? seL4_RISCV_Mega_Page
                                                 : seL4_RISCV_4K_Page;
    for (int i = 0; i < nframes; ++i) {
        seL4_CPtr f = taskman_alloc_empty_slot();
        if (!f) return 0;
        if (qsoe_untyped_retype(ut, type, 0, s_cnode_root, 0, 0, f, 1) != 0) {
            tm_err("tm_mmap_serve(PHYS): device frame retype failed");
            taskman_free_slot(f);
            return 0;
        }
        d->frames[i] = f;
    }
    d->phys = phys; d->len = rlen; d->granule = granule;
    d->nframes = nframes; d->in_use = 1;
    return d;
}

/* Map the registry region's frames covering [phys, phys+len) into proc's
 * VSpace via cnode_copy (shared frame, independent mapping), recording
 * each copy in proc->devframes[] for slot reclamation on exit.  Sets
 * *out_vaddr to the VA of `phys` (its offset into the first frame). */
static int devmap_map_into(tm_process_t *proc, tm_devmap_t *d,
                           unsigned long phys, unsigned long len,
                           unsigned long *out_vaddr)
{
    unsigned long g = d->granule;
    int first = (int)((phys - d->phys) / g);
    int last  = (int)((phys + len - d->phys + g - 1) / g);   /* exclusive */
    if (last > d->nframes) last = d->nframes;

    /* 2 MiB-aligned VA base: a Mega_Page lands on an L1 slot; a 4 KiB run
     * hangs a fresh L0 PT per 2 MiB. */
    unsigned long base_va = (proc->mmap_top + QSOE_MEGA_PAGE - 1) &
                            ~(QSOE_MEGA_PAGE - 1);

    for (int f = first; f < last; ++f) {
        unsigned long va = base_va + (unsigned long)(f - first) * g;

        if (proc->devframe_count >= TM_MAX_DEVFRAMES) {
            tm_err("tm_mmap_serve(PHYS): pid %ld devframe tracker full (cap=%d)",
                   (long)proc->pid, TM_MAX_DEVFRAMES);
            return -ENOMEM;
        }

        if (g == QSOE_PAGE_4K && (va & (QSOE_MEGA_PAGE - 1)) == 0) {
            seL4_CPtr l0 = taskman_alloc_and_retype(
                               seL4_RISCV_PageTableObject, 0);
            if (!l0) { tm_err("tm_mmap_serve(PHYS): L0 PT alloc failed"); return -ENOMEM; }
            if (qsoe_riscv_pagetable_map(l0, proc->vspace, va,
                                         QSOE_VM_ATTR_DEFAULT) != 0) {
                tm_err("tm_mmap_serve(PHYS): L0 PageTable_Map failed");
                return -ENOMEM;
            }
        }

        seL4_CPtr cp = taskman_alloc_empty_slot();
        if (!cp) return -ENOMEM;
        if (qsoe_cnode_copy(s_cnode_root, cp, TM_DEPTH_TASKMAN,
                            s_cnode_root, d->frames[f], TM_DEPTH_TASKMAN,
                            QSOE_RIGHTS_ALL) != 0) {
            tm_err("tm_mmap_serve(PHYS): device frame cnode_copy failed");
            taskman_free_slot(cp);
            return -ENOMEM;
        }
        if (qsoe_riscv_page_map(cp, proc->vspace, va,
                                QSOE_RIGHTS_ALL, QSOE_VM_ATTR_DEFAULT) != 0) {
            tm_err("tm_mmap_serve(PHYS): device frame Page_Map failed");
            qsoe_cnode_delete(s_cnode_root, cp, TM_DEPTH_TASKMAN);
            taskman_free_slot(cp);
            return -ENOMEM;
        }
        proc->devframes[proc->devframe_count++] = cp;
    }

    proc->mmap_top = base_va + (unsigned long)(last - first) * g;
    *out_vaddr = base_va + ((phys - d->phys) - (unsigned long)first * g);
    return 0;
}

/* MAP_PHYS -- map device MMIO into the caller's VSpace.  Find or carve
 * the region in the device-frame registry, then map shared copies in. */
static int mmap_phys(tm_process_t *proc, unsigned long phys,
                     unsigned long len, unsigned long *out_vaddr)
{
    if (phys & (QSOE_PAGE_4K - 1)) {
        tm_err("tm_mmap_serve(PHYS): phys not page-aligned");
        return -EINVAL;
    }
    len = (len + QSOE_PAGE_4K - 1) & ~(QSOE_PAGE_4K - 1);

    tm_devmap_t *d = devmap_find(phys, len);
    if (!d) {
        d = devmap_carve(phys, len);
        if (!d) return -ENOMEM;   /* carve logged the specific reason */
    }
    return devmap_map_into(proc, d, phys, len, out_vaddr);
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
    /* Anonymous megapages are per-process RAM: retype them from the
     * caller's own untyped (growing it on demand) so they are reclaimed
     * when the process exits.  Processes with no pp_ut block (taskman
     * itself, boot init) fall back to the master pool. */
    tm_pput_proc_begin(proc);
    int rc = mmap_anonymous(proc, len, out_vaddr);
    tm_pput_end();
    return rc;
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
            tm_err("tm_munmap_serve: pid %ld va 0x%lx not in tracker",
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
        /* Park the still-valid frame cap for reuse rather than deleting
         * it: the pp_ut allocator can only return this 2 MiB to its
         * parent untyped via a whole-block Revoke at process exit, so a
         * deleted frame's space would be lost until then.  The next mmap
         * re-maps + zeroes it.  On free-list overflow, fall back to the
         * old delete path (that one frame's 2 MiB stays charged to the
         * block until exit -- bounded by TM_MMAP_FREE_MAX, and rare). */
        if (proc->mmap_free_count < TM_MMAP_FREE_MAX) {
            proc->mmap_free[proc->mmap_free_count++] = frm;
        } else {
            (void) qsoe_cnode_delete(s_cnode_root, frm, TM_DEPTH_TASKMAN);
            taskman_free_slot(frm);
        }

        int last = proc->mmap_count - 1;
        if (idx != last) proc->mmap[idx] = proc->mmap[last];
        proc->mmap_count = last;
    }

    /* Rewind the VA cursor when the freed range sat at the top of the
     * mmap region -- the common LIFO case, notably qsh's args page
     * (mmap then immediate munmap on every posix_spawn).  Recycling the
     * frames (above) reclaims the RAM, but mmap_top is bump-only: without
     * this rewind it marches ~2 MiB per spawn into the workers/libc.so
     * region (~0x40000000) after a few hundred spawns, and the next
     * Page_Map there fails.  Non-top frees leave a VA hole (reclaimed at
     * exit); only the contiguous-top case can rewind safely. */
    if (vaddr + bytes == proc->mmap_top) {
        proc->mmap_top = vaddr;
    }
    return 0;
}
