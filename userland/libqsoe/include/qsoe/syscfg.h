/*
 * qsoe/syscfg.h — system-configuration blob format.
 *
 * Public surface shared between taskman (which builds the blob at
 * boot from the FDT) and clients (libqsoe's hwinfo_* wrappers, and
 * any program that needs raw access).  Tag-list layout: a flat
 * sequence of tm_syscfg_tag_t headers, each followed by its payload,
 * terminated by a tag with id = TM_SYSCFG_TAG_END.
 *
 * Naming note: the QRV/QNX-style names hwinfo_find_bus(),
 * hwinfo_find_tag_after(), hwinfo_find_device() are preserved for
 * the PCI server port.  Internally those just walk this blob.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_SYSCFG_H
#define QSOE_SYSCFG_H

#include <qsoe-system.h>     /* uint{16,32,64}_t */

/* Tag header — 4 bytes.  Payload starts immediately after; the next
 * tag begins after `len` payload bytes (no alignment padding).      */
typedef struct {
    uint16_t id;
    uint16_t len;        /* payload bytes, not including this header */
} tm_syscfg_tag_t;

/* Tag IDs.  Values are stable across versions; new tags get fresh
 * IDs and old ones keep their semantics.  TM_SYSCFG_TAG_END (== 0)
 * sentinel must always be the last tag in the blob. */
#define TM_SYSCFG_TAG_END           0
#define TM_SYSCFG_TAG_VERSION       1   /* u32: blob layout version */
#define TM_SYSCFG_TAG_MODEL         2   /* asciz: machine model     */
#define TM_SYSCFG_TAG_COMPATIBLE    3   /* asciz: root compatible[0]*/
#define TM_SYSCFG_TAG_TIMEBASE_HZ   4   /* u64: clock_freq          */
#define TM_SYSCFG_TAG_NUM_CPUS      5   /* u32                      */
#define TM_SYSCFG_TAG_BOOT_HART     6   /* u32                      */
#define TM_SYSCFG_TAG_MEMORY        7   /* (u64 base, u64 size)     */
#define TM_SYSCFG_TAG_PCI_ECAM      8   /* (u64 base, u64 size, u32 lastbus) */
#define TM_SYSCFG_TAG_PCI_IRQ       9   /* 4 * u32: PLIC vectors for INTA/B/C/D */
#define TM_SYSCFG_TAG_PCI_WINDOW   10   /* (u64 cpu, u64 pci, u64 size, u32 flags) */
#define TM_SYSCFG_TAG_DW_MSI       11   /* (u64 dbi_base, u64 dbi_size, u32 plic_irq) */
#define TM_SYSCFG_TAG_UART         12   /* (u64 base, u64 size, u32 irq) */

/* Current layout version reported in TM_SYSCFG_TAG_VERSION. */
#define TM_SYSCFG_VERSION   1

/* PCI-window flag bits (sub-set of QRV HWI_PCI_WINDOW_*). */
#define TM_SYSCFG_PCI_WINDOW_IO         0x1
#define TM_SYSCFG_PCI_WINDOW_MEM        0x2
#define TM_SYSCFG_PCI_WINDOW_PREFETCH   0x4

/* Maximum blob size — bounded by the single-MsgReply payload of 928
 * bytes; we keep some headroom for the reply header.               */
#define TM_SYSCFG_MAX  864

#endif /* QSOE_SYSCFG_H */
