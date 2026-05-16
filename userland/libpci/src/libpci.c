/*
 * libpci.c — QSOE client wrappers for /dev/pci.
 *
 * Lazy-opens /dev/pci on first use; each call marshals a QSOE_PCI_REQ_*
 * opcode through MsgSend.  Replies are unpacked from the IPC buffer's
 * MR layout (mr0/mr1 carry scalars; QSOE_PCI_REQ_READ_BA additionally
 * fills a BAR list starting at byte 32 of the reply payload).
 *
 * The connection is process-wide singleton — like QRV's libpci.  No
 * per-call ConnectAttach.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <pci/pci.h>

#include "sel4_types.h"
#include "qsoe_invoke.h"

extern int            open                     (const char *path, int flags, ...);
extern unsigned long  qsoe_state_coid_to_slot  (int coid);

static int g_pci_fd = -1;

static int ensure_open(void)
{
    if (g_pci_fd >= 0) return g_pci_fd;
    g_pci_fd = open("/dev/pci", 2 /* O_RDWR */, 0);
    return g_pci_fd;
}

/* Reply unpack helpers — read little-endian from a byte buffer. */
static inline uint64_t unpack_u64(const unsigned char *b, unsigned off)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[off + i];
    return v;
}

static inline uint32_t unpack_u32(const unsigned char *b, unsigned off)
{
    return (uint32_t)b[off] |
           ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) |
           ((uint32_t)b[off + 3] << 24);
}

/* Send `label` opcode with mr0/mr1/mr2/mr3, receive reply.  Status
 * (label of reply) is returned via *status; the reply MRs land in
 * rbuf[0..31] (4 words), and further payload (BAR list) in rbuf[32..]. */
static int pci_call(unsigned label, seL4_Word mr0, seL4_Word mr1,
                    seL4_Word mr2, seL4_Word mr3,
                    unsigned char *rbuf, unsigned rcap, int *status)
{
    int fd = ensure_open();
    if (fd < 0) return -1;

    /* Build the seL4 request: place args in MR0..MR3 of qsoe_ipcbuf,
     * label in the MessageInfo.  MsgSend doesn't accept a label arg
     * directly — we go through the lower-level msg.c send path.
     * Easiest: stage args as bytes in a small buffer; MsgSend will
     * pack them.  But labels are needed too — use a custom path. */
    qsoe_ipcbuf->msg[0] = mr0;
    qsoe_ipcbuf->msg[1] = mr1;
    qsoe_ipcbuf->msg[2] = mr2;
    qsoe_ipcbuf->msg[3] = mr3;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(label, 0, 0, 4);
    seL4_CPtr send = qsoe_state_coid_to_slot(fd);
    if (!send) { qsoe_errno = EBADF; return -1; }
    seL4_MessageInfo_t reply = qsoe_sys_call(send, tag,
                                              &qsoe_ipcbuf->msg[0],
                                              &qsoe_ipcbuf->msg[1],
                                              &qsoe_ipcbuf->msg[2],
                                              &qsoe_ipcbuf->msg[3]);
    *status = (int)seL4_MessageInfo_get_label(reply);
    unsigned reply_words = (unsigned)seL4_MessageInfo_get_length(reply);
    unsigned reply_bytes = reply_words * 8;
    if (rcap > 0 && rbuf) {
        unsigned n = reply_bytes < rcap ? reply_bytes : rcap;
        const unsigned char *src = (const unsigned char *)qsoe_ipcbuf->msg;
        for (unsigned i = 0; i < n; ++i) rbuf[i] = src[i];
        /* Zero the remainder so callers reading uninitialised slots
         * see deterministic zero. */
        for (unsigned i = n; i < rcap; ++i) rbuf[i] = 0;
    }
    return 0;
}

/* ---- API ---------------------------------------------------------- */

int pci_bios_present(uint32_t *lastbus, uint32_t *version)
{
    unsigned char r[32];
    int status;
    if (pci_call(QSOE_PCI_REQ_BIOS_PRESENT, 0, 0, 0, 0,
                 r, sizeof r, &status) != 0) return -1;
    if (status != PCI_SUCCESS) { qsoe_errno = EIO; return -1; }
    if (lastbus) *lastbus = (uint32_t)unpack_u64(r, 0);
    if (version) *version = (uint32_t)unpack_u64(r, 8);
    return 0;
}

pci_bdf_t pci_device_find(uint32_t vendor, uint32_t device,
                          uint32_t classcode, uint32_t idx)
{
    unsigned char r[16];
    int status;
    seL4_Word mr0 = ((seL4_Word)vendor << 16) | (seL4_Word)(device & 0xffff);
    seL4_Word mr1 = ((seL4_Word)classcode << 8) | (seL4_Word)(idx & 0xff);
    if (pci_call(QSOE_PCI_REQ_FIND_DEV, mr0, mr1, 0, 0,
                 r, sizeof r, &status) != 0) return PCI_BDF_NONE;
    if (status != PCI_SUCCESS) return PCI_BDF_NONE;
    return (pci_bdf_t)unpack_u64(r, 0);
}

static int cfg_rd(pci_bdf_t bdf, uint16_t off, unsigned size, uint32_t *out)
{
    unsigned char r[16];
    int status;
    seL4_Word mr0 = (seL4_Word)bdf;
    seL4_Word mr1 = ((seL4_Word)size << 16) | (seL4_Word)off;
    if (pci_call(QSOE_PCI_REQ_CFG_RD, mr0, mr1, 0, 0,
                 r, sizeof r, &status) != 0) return -1;
    if (status != PCI_SUCCESS) { qsoe_errno = EIO; return -1; }
    *out = (uint32_t)unpack_u64(r, 0);
    return 0;
}

static int cfg_wr(pci_bdf_t bdf, uint16_t off, unsigned size, uint32_t v)
{
    int status;
    seL4_Word mr0 = (seL4_Word)bdf;
    seL4_Word mr1 = ((seL4_Word)size << 16) | (seL4_Word)off;
    if (pci_call(QSOE_PCI_REQ_CFG_WR, mr0, mr1, (seL4_Word)v, 0,
                 0, 0, &status) != 0) return -1;
    if (status != PCI_SUCCESS) { qsoe_errno = EIO; return -1; }
    return 0;
}

int pci_device_cfg_rd8 (pci_bdf_t bdf, uint16_t off, uint8_t  *out)
{ uint32_t v; if (cfg_rd(bdf, off, 1, &v) != 0) return -1; *out = (uint8_t)v;  return 0; }
int pci_device_cfg_rd16(pci_bdf_t bdf, uint16_t off, uint16_t *out)
{ uint32_t v; if (cfg_rd(bdf, off, 2, &v) != 0) return -1; *out = (uint16_t)v; return 0; }
int pci_device_cfg_rd32(pci_bdf_t bdf, uint16_t off, uint32_t *out)
{ return cfg_rd(bdf, off, 4, out); }

int pci_device_cfg_wr8 (pci_bdf_t bdf, uint16_t off, uint8_t  v) { return cfg_wr(bdf, off, 1, v); }
int pci_device_cfg_wr16(pci_bdf_t bdf, uint16_t off, uint16_t v) { return cfg_wr(bdf, off, 2, v); }
int pci_device_cfg_wr32(pci_bdf_t bdf, uint16_t off, uint32_t v) { return cfg_wr(bdf, off, 4, v); }

int pci_device_read_vid  (pci_bdf_t bdf, uint16_t *vid)   { return pci_device_cfg_rd16(bdf, 0x00, vid);  }
int pci_device_read_did  (pci_bdf_t bdf, uint16_t *did)   { return pci_device_cfg_rd16(bdf, 0x02, did);  }
int pci_device_read_ccode(pci_bdf_t bdf, uint32_t *ccode) { return pci_device_cfg_rd32(bdf, 0x08, ccode);}

int pci_device_attach(pci_bdf_t bdf, uint32_t flags, pci_devhdl_t *hdl)
{
    unsigned char r[16];
    int status;
    if (pci_call(QSOE_PCI_REQ_ATTACH, (seL4_Word)bdf, (seL4_Word)flags,
                 0, 0, r, sizeof r, &status) != 0) return -1;
    if (status != PCI_SUCCESS) { qsoe_errno = EIO; return -1; }
    *hdl = (pci_devhdl_t)unpack_u64(r, 0);
    return 0;
}

int pci_device_detach(pci_devhdl_t hdl)
{
    int status;
    if (pci_call(QSOE_PCI_REQ_DETACH, (seL4_Word)hdl, 0, 0, 0,
                 0, 0, &status) != 0) return -1;
    if (status != PCI_SUCCESS) { qsoe_errno = EINVAL; return -1; }
    return 0;
}

int pci_device_read_ba(pci_devhdl_t hdl, uint32_t *nba, pci_ba_t *ba)
{
    unsigned char r[32 + 6 * 32];
    int status;
    if (pci_call(QSOE_PCI_REQ_READ_BA, (seL4_Word)hdl, 0, 0, 0,
                 r, sizeof r, &status) != 0) return -1;
    if (status != PCI_SUCCESS) { qsoe_errno = EINVAL; return -1; }
    uint32_t n = unpack_u32(r, 0);
    if (n > 6) n = 6;
    *nba = n;
    for (uint32_t i = 0; i < n; ++i) {
        unsigned o = 32 + i * 32;
        ba[i].addr     = unpack_u64(r, o +  0);
        ba[i].size     = unpack_u64(r, o +  8);
        ba[i].bar_num  = unpack_u32(r, o + 16);
        ba[i].type     = unpack_u32(r, o + 20);
        ba[i].flags    = unpack_u32(r, o + 24);
        ba[i].reserved = 0;
    }
    return 0;
}

int pci_device_read_irq(pci_devhdl_t hdl, uint32_t *irq)
{
    unsigned char r[16];
    int status;
    if (pci_call(QSOE_PCI_REQ_READ_IRQ, (seL4_Word)hdl, 0, 0, 0,
                 r, sizeof r, &status) != 0) return -1;
    if (status != PCI_SUCCESS) { qsoe_errno = EIO; return -1; }
    *irq = (uint32_t)unpack_u64(r, 0);
    return 0;
}
