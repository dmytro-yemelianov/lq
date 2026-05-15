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
#include <qsoe-system.h>

/* mmap region origin + page size used to bump tm_process_t.mmap_top. */
#define QSOE_MMAP_BASE  0x2000000UL    /* 32 MiB */
#define QSOE_MEGA_PAGE  0x200000UL     /* 2 MiB  */

int tm_mmap_serve(pid_t caller, unsigned long len, unsigned long *out_vaddr);

#endif /* QSOE_TASKMAN_MEM_H */
