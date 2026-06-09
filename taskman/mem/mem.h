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

/* mmap region origin + page size used to bump tm_process_t.mmap_top. */
#define QSOE_MMAP_BASE  0x2000000UL    /* 32 MiB */
#define QSOE_MEGA_PAGE  0x200000UL     /* 2 MiB  */
#define QSOE_PAGE_4K    0x1000UL       /* 4 KiB  */

/* flags bits passed to tm_mmap_serve.  Maps to libqsoe's QSOE_MAP_*. */
#define TM_MMAP_FLAG_PHYS   0x1u       /* phys arg names a physical addr */

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

/* Bootinfo handle (set once at startup by main.c) — used by
 * MAP_PHYS to walk the device-untyped list. */
void tm_mem_set_bootinfo(seL4_BootInfo *bi);

#endif /* QSOE_TASKMAN_MEM_H */
