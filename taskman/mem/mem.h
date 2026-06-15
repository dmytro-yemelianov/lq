/*
 * mem/mem.h — Memory Manager API.
 *
 * Backs TM_REQ_MMAP: allocates Mega_Page-aligned frames out of
 * taskman's untyped budget and maps them into the caller's VSpace
 * at the next free vaddr in their mmap region (above stack / IPC
 * buffer at 0x1FE000; bumped per-process in tm_process_t.mmap_top).
 */
#ifndef QSOE_TASKMAN_MEM_H
#define QSOE_TASKMAN_MEM_H

#include "../sel4_types.h"
#include <sys/qsoe.h>
#include <qsoe/tm_msgs.h>      /* canonical TM_MMAP_FLAG_PHYS (shared wire flag) */

/* mmap region origin + page size used to bump tm_process_t.mmap_top. */
#define QSOE_MMAP_BASE  0x2000000UL    /* 32 MiB */
#define QSOE_MEGA_PAGE  0x200000UL     /* 2 MiB  */
#define QSOE_PAGE_4K    0x1000UL       /* 4 KiB  */

/* TM_MMAP_FLAG_PHYS now lives in <qsoe/tm_msgs.h> (one definition shared
 * with NQ); it used to be a local 0x1 here, which mismatched the value the
 * shared qsoe_mmap() put on the wire (0x10000) and routed device mappings
 * to the anonymous allocator. */

/* mmap allocator.  `flags` distinguishes anonymous (default) from
 * MAP_PHYS (physical-memory window for MMIO).  Anonymous uses
 * 2 MiB Mega_Pages from RAM untyped; MAP_PHYS retypes the
 * device-untyped covering `phys` into 4 KiB Pages.  In both cases
 * the caller gets a bump-allocated VA range back in *out_vaddr.  */
int tm_mmap_serve(pid_t caller, unsigned long len, unsigned long flags,
                  unsigned long phys, unsigned long *out_vaddr);

/* munmap counterpart to tm_mmap_serve.  Tears down every Mega_Page
 * in [vaddr, vaddr+len): Page_Unmap, CNode_Delete, recycle the slot,
 * and drop the entry from proc->mmap[].  Strict: every page in the
 * range MUST be present in the tracker; otherwise -EINVAL. */
int tm_munmap_serve(pid_t caller, unsigned long vaddr, unsigned long len);

/* TM_REQ_MPROTECT: change the PROT_* rights on an already-mapped range.
 * Real for the pages rtld re-protects: RELRO pages tracked in proc->mprot[]
 * are re-Page_Map'd with the new rights; an anonymous mmap Mega_Page that
 * already satisfies the request (it is mapped R|W) is accepted as-is.  addr
 * must be page-aligned, len > 0.  Returns 0, or a negative errno; an
 * untracked range or a request unsatisfiable at the page's granularity is
 * reported loudly (never a silent no-op). */
int tm_mprotect_serve(pid_t caller, unsigned long addr, unsigned long len,
                      unsigned long prot);

/* TM_REQ_ALLOC_PHYS: map one anonymous RAM page and report its VA + PA
 * (for drivers that must hand a frame's physical address to hardware,
 * e.g. the DesignWare PCIe MSI trap target).  See mmap.c. */
int tm_alloc_phys_serve(pid_t caller, unsigned long length, unsigned prot,
                        unsigned long *out_vaddr, unsigned long *out_paddr);

/* Bootinfo handle (set once at startup by main.c) — used by
 * MAP_PHYS to walk the device-untyped list. */
void tm_mem_set_bootinfo(seL4_BootInfo *bi);

#endif /* QSOE_TASKMAN_MEM_H */
