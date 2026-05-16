/*
 * sys/rsrcdbmgr.h — QRV/QNX-compatible Resource Manager Database API.
 *
 * Centralized in-taskman database of hardware resources: physical
 * memory ranges, IRQs, I/O ports, DMA channels, and PCI MMIO sub-
 * windows.  Seeded at boot from the FDT/syscfg; manipulated at
 * runtime by drivers and the PCI server.
 *
 * Layered over the TM_REQ_RSRC_* wire ops in <qsoe/wire.h>.
 *
 * Compatible with QRV's surface so ported drivers
 *   #include <sys/rsrcdbmgr.h>
 *   rsrcdbmgr_attach(&req, 1);
 * compile unchanged.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_SYS_RSRCDBMGR_H
#define QSOE_SYS_RSRCDBMGR_H

#include <qsoe-system.h>

/* ------------------------------------------------------------------
 * Type / flag bits — low byte = class, upper bytes = flag.
 * Same numeric values as QRV's RSRCDBMGR_* so untouched header users
 * keep working.
 * ------------------------------------------------------------------ */
#define RSRCDBMGR_TYPE_MASK         0xffU

/* Resource classes (low byte of `flags`). */
#define RSRCDBMGR_MEMORY            0U   /* physical memory range  */
#define RSRCDBMGR_IRQ               1U   /* IRQ source number      */
#define RSRCDBMGR_IO_PORT           2U   /* x86 I/O port           */
#define RSRCDBMGR_DMA_CHANNEL       3U   /* DMA channel number     */
#define RSRCDBMGR_PCI_MEMORY        4U   /* PCI MMIO sub-window    */
#define RSRCDBMGR_TYPE_COUNT        8U   /* upper bound (3 spare)  */

/* Allocation / behavioural flags (upper bytes). */
#define RSRCDBMGR_FLAG_MASK         0xffffff00U
#define RSRCDBMGR_FLAG_USED         0x00000100U  /* entry is currently allocated */
#define RSRCDBMGR_FLAG_ALIGN        0x00000200U  /* .align is valid             */
#define RSRCDBMGR_FLAG_RANGE        0x00000400U  /* .start/.end are valid       */
#define RSRCDBMGR_FLAG_SHARE        0x00000800U  /* multi-owner allowed         */
#define RSRCDBMGR_FLAG_TOPDOWN      0x00001000U  /* search high to low          */
#define RSRCDBMGR_FLAG_NAME         0x00002000U  /* .name is valid              */

/* ------------------------------------------------------------------
 * Wire structures.  Same byte layout as QRV's structs in
 * include/sys/rsrcdbmgr.h so existing callers don't need refactoring.
 * ------------------------------------------------------------------ */
typedef struct rsrc_alloc {
    uint64_t    start;
    uint64_t    end;
    uint32_t    flags;       /* class | RSRCDBMGR_FLAG_* */
    const char *name;
} rsrc_alloc_t;

typedef struct rsrc_request {
    uint64_t    length;
    uint64_t    align;
    uint64_t    start;
    uint64_t    end;
    uint32_t    flags;
    uint32_t    zero[2];
    const char *name;
} rsrc_request_t;

/* ------------------------------------------------------------------
 * Public API — same signatures as QRV's.
 * ------------------------------------------------------------------ */
int rsrcdbmgr_create (rsrc_alloc_t   *items, unsigned count);
int rsrcdbmgr_destroy(rsrc_alloc_t   *items, unsigned count);
int rsrcdbmgr_attach (rsrc_request_t *list,  unsigned count);
int rsrcdbmgr_detach (rsrc_request_t *list,  unsigned count);

/* Query: dump up to `listcnt` entries starting at index `start` that
 * match the class given in `type`'s low byte.  Returns the number of
 * entries written (or, if list==NULL, the total matching count).   */
int rsrcdbmgr_query  (rsrc_alloc_t *list, int listcnt, int start,
                      uint32_t type);

#endif /* QSOE_SYS_RSRCDBMGR_H */
