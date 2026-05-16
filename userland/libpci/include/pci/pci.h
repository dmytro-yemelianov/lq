/*
 * pci/pci.h — QSOE PCI client library public header.
 *
 * Programs talk to /dev/pci through this API.  Implementation is in
 * libpci.a; the actual wire protocol used between libpci and the
 * /sbin/pci-server resource manager is defined further below
 * (QSOE_PCI_REQ_*).
 *
 * API surface is QNX/QRV-shaped so future driver ports flow with
 * minimal edits, but the v0.8-rc2 server only implements the basic
 * scan/config/BAR/IRQ operations (no MSI / MSI-X — those land in
 * v0.8-rc3 along with the DesignWare MSI controller).
 *
 * Copyright (c) 2008, 2009 QNX Software Systems  (original libpci shape)
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_PCI_PCI_H
#define QSOE_PCI_PCI_H

#include <qsoe-system.h>

/* ---- Identifier helpers ---------------------------------------- */

/* Bus / device / function tuple packed into a 32-bit word.  We keep
 * the QRV/QNX accessor layout: bits 16-23 bus, 11-15 device, 8-10
 * function.  Low byte is reserved for ARI (v0.9).  Macros do raw
 * shifts; helpers below return signed -1 on errors. */
typedef uint32_t pci_bdf_t;

#define PCI_BUS(bdf)   (((bdf) >> 16) & 0xffu)
#define PCI_DEV(bdf)   (((bdf) >> 11) & 0x1fu)
#define PCI_FUNC(bdf)  (((bdf) >>  8) & 0x07u)
#define PCI_BDF(b,d,f) (((uint32_t)((b) & 0xff) << 16) | \
                        ((uint32_t)((d) & 0x1f) << 11) | \
                        ((uint32_t)((f) & 0x07) <<  8))

#define PCI_BDF_NONE   ((pci_bdf_t)0xffffffffu)

/* Opaque per-process attachment handle.  Returned by pci_device_attach()
 * and passed to subsequent calls.  Zero indicates a failed attach. */
typedef uint32_t pci_devhdl_t;

/* ---- Status codes ---------------------------------------------- */

enum {
    PCI_SUCCESS              = 0,
    PCI_ERR_NOT_FOUND        = 1,
    PCI_ERR_ENXIO            = 2,
    PCI_ERR_EBUSY            = 3,
    PCI_ERR_EINVAL           = 4,
    PCI_ERR_ENOSYS           = 5,    /* op not implemented in this rc */
};

/* ---- BAR description ------------------------------------------- */

#define PCI_BAR_TYPE_NONE   0
#define PCI_BAR_TYPE_IO     1
#define PCI_BAR_TYPE_MEM32  2
#define PCI_BAR_TYPE_MEM64  3

#define PCI_BAR_FLAG_PREFETCH  0x1u

typedef struct {
    uint64_t addr;       /* CPU phys address (or PCI IO addr for IO BARs) */
    uint64_t size;       /* bytes; 0 if BAR unimplemented */
    uint32_t bar_num;    /* 0..5 */
    uint32_t type;       /* PCI_BAR_TYPE_* */
    uint32_t flags;      /* PCI_BAR_FLAG_* */
    uint32_t reserved;
} pci_ba_t;

/* ---- Public API ------------------------------------------------- */

/* Returns 0 on success and fills `*lastbus` / `*version`.  `*lastbus`
 * is the highest occupied bus number.  `*version` is the QSOE
 * pci-server wire version (currently 1). */
int  pci_bios_present(uint32_t *lastbus, uint32_t *version);

/* Find a device by vendor/device ID.  Pass 0xffff for a wildcard.
 * idx selects the N-th match.  Returns PCI_BDF_NONE if no match. */
pci_bdf_t pci_device_find(uint32_t vendor, uint32_t device,
                          uint32_t classcode, uint32_t idx);

/* Configuration-space accessors (BDF-keyed; no handle required).    */
int  pci_device_cfg_rd8 (pci_bdf_t bdf, uint16_t off, uint8_t  *out);
int  pci_device_cfg_rd16(pci_bdf_t bdf, uint16_t off, uint16_t *out);
int  pci_device_cfg_rd32(pci_bdf_t bdf, uint16_t off, uint32_t *out);
int  pci_device_cfg_wr8 (pci_bdf_t bdf, uint16_t off, uint8_t   v);
int  pci_device_cfg_wr16(pci_bdf_t bdf, uint16_t off, uint16_t  v);
int  pci_device_cfg_wr32(pci_bdf_t bdf, uint16_t off, uint32_t  v);

/* Convenience wrappers on common cfg registers. */
int  pci_device_read_vid  (pci_bdf_t bdf, uint16_t *vid);
int  pci_device_read_did  (pci_bdf_t bdf, uint16_t *did);
int  pci_device_read_ccode(pci_bdf_t bdf, uint32_t *ccode);

/* Attach to a device.  Returns 0 on success and stores the handle in
 * *hdl.  The handle is per-process; pci_device_detach releases it. */
int  pci_device_attach(pci_bdf_t bdf, uint32_t flags, pci_devhdl_t *hdl);
int  pci_device_detach(pci_devhdl_t hdl);

/* Read the populated BARs for an attached device.  Fills *nba with
 * the count of populated entries (0..6) and writes ba[0..*nba-1]. */
int  pci_device_read_ba(pci_devhdl_t hdl, uint32_t *nba, pci_ba_t *ba);

/* Read the attached device's interrupt (PLIC vector — already includes
 * QSOE_PLIC_VECTOR_BASE offset).  -1 if no IRQ. */
int  pci_device_read_irq(pci_devhdl_t hdl, uint32_t *irq);

/* ---- Wire protocol --------------------------------------------- */

/* Request opcodes carried in the seL4_MessageInfo label.  Reply label
 * carries PCI_SUCCESS or PCI_ERR_*. */
enum {
    QSOE_PCI_REQ_BIOS_PRESENT  = 0x01,
    QSOE_PCI_REQ_FIND_DEV      = 0x02,
    QSOE_PCI_REQ_CFG_RD        = 0x03,
    QSOE_PCI_REQ_CFG_WR        = 0x04,
    QSOE_PCI_REQ_ATTACH        = 0x05,
    QSOE_PCI_REQ_DETACH        = 0x06,
    QSOE_PCI_REQ_READ_BA       = 0x07,
    QSOE_PCI_REQ_READ_IRQ      = 0x08,
};

#define QSOE_PCI_WIRE_VERSION  1

#endif /* QSOE_PCI_PCI_H */
