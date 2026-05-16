/*
 * /sbin/pci-server — QSOE PCI resource manager.
 *
 * v0.8-rc2 scope: single-bus generic ECAM (qemu-virt
 * "pci-host-ecam-generic"), INTx routing via four PLIC vectors,
 * client API serving QSOE_PCI_REQ_* over /dev/pci.  No MSI / MSI-X
 * (v0.8-rc3), no recursive bridge configuration, no hotplug.
 *
 * Architecture:
 *   - At boot, fetch PCI_ECAM / PCI_WINDOW / PCI_IRQ tags from
 *     taskman's syscfg blob (qsoe/hwinfo.h).
 *   - qsoe_mmap(MAP_PHYS) the ECAM window read/write.
 *   - Walk bus 0, slots 0..31, function 0 (and 1..7 if multi-function
 *     bit set), building a small static device table.
 *   - ChannelCreate + pathmgr_register("/dev/pci") then enter the
 *     standard MsgReceive/MsgReply loop modelled on slogger / ser8250.
 *
 * Per CLAUDE.md "Requirements for resource managers" — only libqsoe
 * + libc; no direct seL4 syscalls.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <qsoe/hwinfo.h>
#include <qsoe/syscfg.h>
#include <pci/pci.h>
#include <sys/slog.h>
#include <sys/slogcodes.h>

#include "../../taskman/sel4_types.h"

#define PCI_PATH         "/dev/pci"
#define PCI_MAX_DEVS     64
#define PCI_MAX_HANDLES  32

/* Each device discovered at boot lives in this table.  Indexed by
 * (bus, devfn) via a linear search — fine for v0.8-rc2 device counts. */
typedef struct {
    pci_bdf_t bdf;
    uint16_t  vid;
    uint16_t  did;
    uint32_t  ccode;       /* class << 16 | subclass << 8 | progif */
    uint32_t  irq;         /* PLIC vector incl. QSOE_PLIC_VECTOR_BASE; 0 if none */
    uint8_t   intpin;      /* 1=INTA..4=INTD, 0=none */
    uint8_t   header_type;
    uint8_t   multifunc;
    uint8_t   _pad;
    pci_ba_t  ba[6];
    uint32_t  nba;
} pci_dev_t;

static pci_dev_t g_devs[PCI_MAX_DEVS];
static unsigned  g_ndevs;
static uint8_t   g_lastbus;

/* Per-client attach handles.  A handle is just (1 + index) into this
 * table; the table tracks which (badge, devidx) owns it so detach can
 * verify the caller. */
typedef struct {
    uint32_t badge;
    uint32_t devidx;
    uint32_t flags;
    uint32_t in_use;
} pci_handle_t;

static pci_handle_t g_handles[PCI_MAX_HANDLES];

/* ---- ECAM bring-up -------------------------------------------------- */

static volatile uint8_t *g_ecam;
static uint64_t          g_ecam_size;
static uint8_t           g_ecam_lastbus;

/* INTx PLIC vectors (raw — caller adds QSOE_PLIC_VECTOR_BASE). */
static uint32_t g_intx[4];

static inline volatile uint8_t *cfg_ptr(pci_bdf_t bdf, uint32_t off)
{
    /* ECAM address = base + (bus << 20) + (devfn << 12) + off. */
    uint32_t bus  = PCI_BUS(bdf);
    uint32_t dev  = PCI_DEV(bdf);
    uint32_t func = PCI_FUNC(bdf);
    uint64_t offset = ((uint64_t)bus << 20) |
                      ((uint64_t)(dev << 3 | func) << 12) |
                      (uint64_t)(off & 0xfff);
    if (offset >= g_ecam_size) return 0;
    return g_ecam + offset;
}

/* Raw cfg-space accessors. */
static int cfg_rd32(pci_bdf_t bdf, uint16_t off, uint32_t *out)
{
    volatile uint8_t *p = cfg_ptr(bdf, off & ~3u);
    if (!p) return -1;
    *out = *(volatile uint32_t *)p;
    return 0;
}

static int cfg_rd16(pci_bdf_t bdf, uint16_t off, uint16_t *out)
{
    uint32_t v;
    if (cfg_rd32(bdf, off, &v) != 0) return -1;
    *out = (uint16_t)(v >> ((off & 2) * 8));
    return 0;
}

static int cfg_rd8(pci_bdf_t bdf, uint16_t off, uint8_t *out)
{
    uint32_t v;
    if (cfg_rd32(bdf, off, &v) != 0) return -1;
    *out = (uint8_t)(v >> ((off & 3) * 8));
    return 0;
}

static int cfg_wr32(pci_bdf_t bdf, uint16_t off, uint32_t v)
{
    volatile uint8_t *p = cfg_ptr(bdf, off & ~3u);
    if (!p) return -1;
    *(volatile uint32_t *)p = v;
    return 0;
}

static int cfg_wr16(pci_bdf_t bdf, uint16_t off, uint16_t v)
{
    uint32_t orig;
    if (cfg_rd32(bdf, off, &orig) != 0) return -1;
    unsigned sh = (off & 2) * 8;
    orig = (orig & ~(0xffffu << sh)) | ((uint32_t)v << sh);
    return cfg_wr32(bdf, off, orig);
}

static int cfg_wr8(pci_bdf_t bdf, uint16_t off, uint8_t v)
{
    uint32_t orig;
    if (cfg_rd32(bdf, off, &orig) != 0) return -1;
    unsigned sh = (off & 3) * 8;
    orig = (orig & ~(0xffu << sh)) | ((uint32_t)v << sh);
    return cfg_wr32(bdf, off, orig);
}

/* ---- ECAM map + INTx table fetch ---------------------------------- */

static int ecam_init(void)
{
    /* Pull PCI_ECAM tag from syscfg. */
    qsoe_hwi_cursor_t c = hwi_find_tag(TM_SYSCFG_TAG_PCI_ECAM);
    if (c < 0) {
        slogf(_SLOGC_PCI, _SLOG_ERROR,
              "pci-server: no PCI_ECAM tag — host bridge not in FDT?");
        return -1;
    }
    unsigned char ebuf[20];
    int paylen = hwi_tag_payload(c, ebuf, sizeof ebuf);
    if (paylen < 20) return -1;
    uint64_t ecam_base = 0;
    for (int b = 7; b >= 0; --b) ecam_base = (ecam_base << 8) | ebuf[b];
    uint64_t ecam_size = 0;
    for (int b = 15; b >= 8; --b) ecam_size = (ecam_size << 8) | ebuf[b];
    g_ecam_lastbus = ebuf[16];

    slogf(_SLOGC_PCI, _SLOG_INFO,
          "pci-server: ecam base=0x%x size=0x%x lastbus=%u",
          (unsigned)ecam_base, (unsigned)ecam_size, g_ecam_lastbus);

    /* v0.8-rc2 caps the ECAM mapping to a single bus (= 1 MiB).
     * Multi-bus / bridge scanning lands in rc3 with a larger
     * mapping.  Cap to 2 MiB so it rounds to a Mega_Page. */
    unsigned long ecam_map_size = ecam_size;
    if (ecam_map_size > 0x200000UL) ecam_map_size = 0x200000UL;

    /* Map a slice of the ECAM window read/write.  prot/flags beyond
     * MAP_PHYS are ignored by qsoe_mmap today; PROT_NOCACHE for MMIO
     * is implicit. */
    void *va = qsoe_mmap(0, ecam_map_size,
                         3 /* PROT_READ|PROT_WRITE */,
                         QSOE_MAP_PHYS, -1,
                         (long)ecam_base);
    if (!va || va == QSOE_MAP_FAILED) {
        slogf(_SLOGC_PCI, _SLOG_ERROR, "pci-server: ECAM mmap failed");
        return -1;
    }
    g_ecam      = (volatile uint8_t *)va;
    g_ecam_size = ecam_size;

    /* Pull INTx vectors (optional — best-effort). */
    qsoe_hwi_cursor_t ic = hwi_find_tag(TM_SYSCFG_TAG_PCI_IRQ);
    if (ic >= 0) {
        unsigned char ibuf[16];
        if (hwi_tag_payload(ic, ibuf, sizeof ibuf) >= 16) {
            for (int i = 0; i < 4; ++i) {
                g_intx[i] = (uint32_t)ibuf[i * 4 + 0] |
                            ((uint32_t)ibuf[i * 4 + 1] << 8) |
                            ((uint32_t)ibuf[i * 4 + 2] << 16) |
                            ((uint32_t)ibuf[i * 4 + 3] << 24);
            }
            slogf(_SLOGC_PCI, _SLOG_INFO,
                  "pci-server: intx PLIC vectors %u/%u/%u/%u",
                  g_intx[0], g_intx[1], g_intx[2], g_intx[3]);
        }
    }
    return 0;
}

/* ---- Bus enumeration ---------------------------------------------- */

static void probe_function(pci_bdf_t bdf, int is_func0)
{
    if (g_ndevs >= PCI_MAX_DEVS) return;
    uint32_t id;
    if (cfg_rd32(bdf, 0x00, &id) != 0) return;
    uint16_t vid = (uint16_t)(id & 0xffff);
    if (vid == 0xffff) return;          /* unimplemented function */
    uint16_t did = (uint16_t)(id >> 16);

    uint32_t ccode_word;
    cfg_rd32(bdf, 0x08, &ccode_word);
    uint32_t ccode = ccode_word >> 8;   /* (class<<16 | subclass<<8 | progif) */
    uint8_t htype = 0;
    cfg_rd8(bdf, 0x0e, &htype);
    int multifunc = (htype & 0x80) ? 1 : 0;

    pci_dev_t *d = &g_devs[g_ndevs++];
    d->bdf         = bdf;
    d->vid         = vid;
    d->did         = did;
    d->ccode       = ccode;
    d->header_type = htype & 0x7f;
    d->multifunc   = (uint8_t)multifunc;
    d->intpin      = 0;
    d->irq         = 0;
    d->nba         = 0;

    /* INTx pin (cfg+0x3D) — 1..4 means INTA..INTD; 0 = no INTx. */
    uint8_t intpin = 0;
    cfg_rd8(bdf, 0x3d, &intpin);
    if (intpin >= 1 && intpin <= 4) {
        d->intpin = intpin;
        uint32_t plic = g_intx[intpin - 1];
        d->irq = plic ? (plic + QSOE_PLIC_VECTOR_BASE) : 0;
    }

    /* BARs: cfg+0x10..0x24 (six 32-bit BARs).  Only valid for
     * header_type=0 (regular device); type 1 (bridge) has 2 BARs.
     * v0.8-rc2 scans regular devices only. */
    if ((htype & 0x7f) == 0) {
        for (int i = 0; i < 6; ++i) {
            uint32_t bar;
            cfg_rd32(bdf, (uint16_t)(0x10 + i * 4), &bar);
            if (bar == 0) continue;
            pci_ba_t *b = &d->ba[d->nba++];
            b->bar_num = (uint32_t)i;
            b->reserved = 0;
            b->flags = 0;
            if (bar & 1) {
                b->type = PCI_BAR_TYPE_IO;
                b->addr = bar & ~0x3ull;
            } else {
                int is_64 = ((bar >> 1) & 0x3) == 2;
                if (bar & 0x8) b->flags |= PCI_BAR_FLAG_PREFETCH;
                if (is_64 && i + 1 < 6) {
                    uint32_t hi;
                    cfg_rd32(bdf, (uint16_t)(0x10 + (i + 1) * 4), &hi);
                    b->type = PCI_BAR_TYPE_MEM64;
                    b->addr = ((uint64_t)hi << 32) | (bar & ~0xfull);
                    /* Skip the second BAR slot. */
                    ++i;
                } else {
                    b->type = PCI_BAR_TYPE_MEM32;
                    b->addr = bar & ~0xfull;
                }
            }
            /* BAR size by write-all-ones / read-back trick — skipped
             * for v0.8-rc2 (qemu virtio BARs are sized by the firmware
             * already and we just report the programmed address).
             * Real sizing lands when devb-nvme needs it. */
            b->size = 0;
        }
    }

    slogf(_SLOGC_PCI, _SLOG_INFO,
          "pci-server:   %x:%x:%x  %x:%x  class=%x  intx=%c  irq=%u",
          (unsigned)PCI_BUS(bdf),  (unsigned)PCI_DEV(bdf),
          (unsigned)PCI_FUNC(bdf), (unsigned)vid, (unsigned)did,
          (unsigned)ccode,
          (char)(intpin ? ('A' + intpin - 1) : '-'),
          (unsigned)d->irq);

    if (is_func0 && !multifunc) {
        /* Single-function device — skip funcs 1..7. */
    }
}

static void scan_bus(uint8_t bus)
{
    for (uint8_t dev = 0; dev < 32; ++dev) {
        pci_bdf_t bdf0 = PCI_BDF(bus, dev, 0);
        /* Probe function 0 first to learn multifunc status. */
        unsigned before = g_ndevs;
        probe_function(bdf0, 1);
        if (g_ndevs == before) continue;
        if (!g_devs[g_ndevs - 1].multifunc) continue;
        for (uint8_t func = 1; func < 8; ++func) {
            probe_function(PCI_BDF(bus, dev, func), 0);
        }
    }
}

static void scan_all(void)
{
    g_ndevs = 0;
    /* v0.8-rc2: bus 0 only.  Recursive bridge scan lives in rc3. */
    scan_bus(0);
    g_lastbus = 0;
    slogf(_SLOGC_PCI, _SLOG_INFO,
          "pci-server: scan complete, %u devices on bus 0", g_ndevs);
}

/* ---- Helpers ------------------------------------------------------ */

static pci_dev_t *find_by_bdf(pci_bdf_t bdf)
{
    for (unsigned i = 0; i < g_ndevs; ++i) {
        if (g_devs[i].bdf == bdf) return &g_devs[i];
    }
    return 0;
}

static int alloc_handle(uint32_t badge, uint32_t devidx, uint32_t flags)
{
    for (int i = 0; i < PCI_MAX_HANDLES; ++i) {
        if (!g_handles[i].in_use) {
            g_handles[i].badge  = badge;
            g_handles[i].devidx = devidx;
            g_handles[i].flags  = flags;
            g_handles[i].in_use = 1;
            return i + 1;                /* 0 reserved as "invalid" */
        }
    }
    return 0;
}

static pci_handle_t *get_handle(uint32_t hdl)
{
    if (hdl == 0 || hdl > PCI_MAX_HANDLES) return 0;
    pci_handle_t *h = &g_handles[hdl - 1];
    if (!h->in_use) return 0;
    return h;
}

/* ---- Wire dispatch ------------------------------------------------ */

/* Reply buffer used for payload-bearing replies (BAR list).  Reuses
 * the IPC buffer via qsoe_ipcbuf->msg[4..]. */

static seL4_Word handle_find_dev(seL4_Word mr0, seL4_Word mr1,
                                 seL4_Word *out_mr0, seL4_Word *out_mr1,
                                 unsigned *out_len)
{
    (void)out_mr1;          /* find-dev returns BDF in mr0 only */
    /* mr0: vendor<<16 | device  ; mr1: classcode<<8 | idx (idx in low 8) */
    uint32_t vid   = (uint32_t)(mr0 >> 16) & 0xffff;
    uint32_t did   = (uint32_t)mr0 & 0xffff;
    uint32_t ccode = (uint32_t)(mr1 >> 8);
    uint32_t idx   = (uint32_t)mr1 & 0xff;

    unsigned matches = 0;
    for (unsigned i = 0; i < g_ndevs; ++i) {
        pci_dev_t *d = &g_devs[i];
        if (vid != 0xffff && d->vid != vid) continue;
        if (did != 0xffff && d->did != did) continue;
        if (ccode != 0 && (d->ccode >> 8) != ccode) continue;
        if (matches++ == idx) {
            *out_mr0 = (seL4_Word)d->bdf;
            *out_len = 1;
            return PCI_SUCCESS;
        }
    }
    *out_mr0 = (seL4_Word)PCI_BDF_NONE;
    *out_len = 1;
    return PCI_ERR_NOT_FOUND;
}

static seL4_Word handle_cfg_rd(seL4_Word mr0, seL4_Word mr1,
                               seL4_Word *out_mr0, unsigned *out_len)
{
    /* mr0: bdf ; mr1: size<<16 | off */
    pci_bdf_t bdf = (pci_bdf_t)mr0;
    uint32_t off  = mr1 & 0xffff;
    uint32_t sz   = (mr1 >> 16) & 0xff;
    if (!find_by_bdf(bdf)) return PCI_ERR_NOT_FOUND;
    uint32_t v = 0;
    switch (sz) {
    case 1: { uint8_t  b; if (cfg_rd8 (bdf, off, &b) != 0) return PCI_ERR_ENXIO; v = b; break; }
    case 2: { uint16_t w; if (cfg_rd16(bdf, off, &w) != 0) return PCI_ERR_ENXIO; v = w; break; }
    case 4: { uint32_t d; if (cfg_rd32(bdf, off, &d) != 0) return PCI_ERR_ENXIO; v = d; break; }
    default: return PCI_ERR_EINVAL;
    }
    *out_mr0 = (seL4_Word)v;
    *out_len = 1;
    return PCI_SUCCESS;
}

static seL4_Word handle_cfg_wr(seL4_Word mr0, seL4_Word mr1, seL4_Word mr2)
{
    pci_bdf_t bdf = (pci_bdf_t)mr0;
    uint32_t off  = mr1 & 0xffff;
    uint32_t sz   = (mr1 >> 16) & 0xff;
    if (!find_by_bdf(bdf)) return PCI_ERR_NOT_FOUND;
    switch (sz) {
    case 1: return (cfg_wr8 (bdf, off, (uint8_t)mr2)  == 0) ? PCI_SUCCESS : PCI_ERR_ENXIO;
    case 2: return (cfg_wr16(bdf, off, (uint16_t)mr2) == 0) ? PCI_SUCCESS : PCI_ERR_ENXIO;
    case 4: return (cfg_wr32(bdf, off, (uint32_t)mr2) == 0) ? PCI_SUCCESS : PCI_ERR_ENXIO;
    default: return PCI_ERR_EINVAL;
    }
}

static seL4_Word handle_attach(uint32_t badge, seL4_Word mr0, seL4_Word mr1,
                               seL4_Word *out_mr0, unsigned *out_len)
{
    pci_bdf_t bdf = (pci_bdf_t)mr0;
    uint32_t flags = (uint32_t)mr1;
    pci_dev_t *d = find_by_bdf(bdf);
    if (!d) return PCI_ERR_NOT_FOUND;
    unsigned idx = (unsigned)(d - g_devs);
    int h = alloc_handle(badge, idx, flags);
    if (!h) return PCI_ERR_EBUSY;
    *out_mr0 = (seL4_Word)h;
    *out_len = 1;
    return PCI_SUCCESS;
}

static seL4_Word handle_detach(uint32_t badge, seL4_Word mr0)
{
    pci_handle_t *h = get_handle((uint32_t)mr0);
    if (!h) return PCI_ERR_EINVAL;
    if (h->badge != badge) return PCI_ERR_EINVAL;
    h->in_use = 0;
    return PCI_SUCCESS;
}

static seL4_Word handle_read_irq(uint32_t badge, seL4_Word mr0,
                                 seL4_Word *out_mr0, unsigned *out_len)
{
    pci_handle_t *h = get_handle((uint32_t)mr0);
    if (!h || h->badge != badge) return PCI_ERR_EINVAL;
    pci_dev_t *d = &g_devs[h->devidx];
    *out_mr0 = (seL4_Word)d->irq;
    *out_len = 1;
    return d->irq ? PCI_SUCCESS : PCI_ERR_NOT_FOUND;
}

/* ---- Main loop ---------------------------------------------------- */

/* Pack a u64 little-endian into a byte buffer at offset `off`. */
static inline void pack_u64(unsigned char *buf, unsigned off, uint64_t v)
{
    for (int b = 0; b < 8; ++b) buf[off + b] = (unsigned char)(v >> (b * 8));
}

/* Pack a u32 little-endian. */
static inline void pack_u32(unsigned char *buf, unsigned off, uint32_t v)
{
    for (int b = 0; b < 4; ++b) buf[off + b] = (unsigned char)(v >> (b * 8));
}

static void serve(int chid)
{
    /* Reply buffer big enough for the worst-case payload — 6 BARs ×
     * 32 bytes/BAR = 192 bytes + header.  Round to 256. */
    unsigned char rbuf[256];

    for (;;) {
        struct _msg_info info;
        unsigned char dummy[8];
        int rcvid = MsgReceive(chid, dummy, sizeof dummy, &info);
        if (rcvid < 0) continue;
        if (info.flags & QSOE_MI_PULSE) continue;

        seL4_Word mr0 = qsoe_ipcbuf->msg[0];
        seL4_Word mr1 = qsoe_ipcbuf->msg[1];
        seL4_Word mr2 = qsoe_ipcbuf->msg[2];
        unsigned op    = info.label;
        uint32_t badge = (uint32_t)info.scoid;

        seL4_Word out_mr0 = 0, out_mr1 = 0;
        unsigned out_bytes = 0;
        int status = PCI_ERR_ENOSYS;

        switch (op) {
        case QSOE_PCI_REQ_BIOS_PRESENT:
            out_mr0    = (seL4_Word)g_lastbus;
            out_mr1    = (seL4_Word)QSOE_PCI_WIRE_VERSION;
            out_bytes  = 16;     /* mr0 + mr1 */
            status     = PCI_SUCCESS;
            break;
        case QSOE_PCI_REQ_FIND_DEV: {
            unsigned r;
            status = (int)handle_find_dev(mr0, mr1, &out_mr0, &out_mr1, &r);
            out_bytes = r * 8;
            break;
        }
        case QSOE_PCI_REQ_CFG_RD: {
            unsigned r;
            status = (int)handle_cfg_rd(mr0, mr1, &out_mr0, &r);
            out_bytes = r * 8;
            break;
        }
        case QSOE_PCI_REQ_CFG_WR:
            status = (int)handle_cfg_wr(mr0, mr1, mr2);
            break;
        case QSOE_PCI_REQ_ATTACH: {
            unsigned r;
            status = (int)handle_attach(badge, mr0, mr1, &out_mr0, &r);
            out_bytes = r * 8;
            break;
        }
        case QSOE_PCI_REQ_DETACH:
            status = (int)handle_detach(badge, mr0);
            break;
        case QSOE_PCI_REQ_READ_BA: {
            /* Special-case: nba in out_mr0, BAR list packed into rbuf
             * starting at byte 32 (after the 4-MR header). */
            pci_handle_t *h = get_handle((uint32_t)mr0);
            if (!h || h->badge != badge) {
                status = PCI_ERR_EINVAL;
                break;
            }
            pci_dev_t *d = &g_devs[h->devidx];
            for (int i = 0; i < 32; ++i) rbuf[i] = 0;
            for (unsigned i = 0; i < d->nba; ++i) {
                unsigned o = 32 + i * 32;
                pack_u64(rbuf, o +  0, d->ba[i].addr);
                pack_u64(rbuf, o +  8, d->ba[i].size);
                pack_u32(rbuf, o + 16, d->ba[i].bar_num);
                pack_u32(rbuf, o + 20, d->ba[i].type);
                pack_u32(rbuf, o + 24, d->ba[i].flags);
                pack_u32(rbuf, o + 28, 0);
            }
            pack_u32(rbuf, 0, d->nba);
            MsgReply(rcvid, PCI_SUCCESS, rbuf, 32 + (int)(d->nba * 32));
            continue;
        }
        case QSOE_PCI_REQ_READ_IRQ: {
            unsigned r;
            status = (int)handle_read_irq(badge, mr0, &out_mr0, &r);
            out_bytes = r * 8;
            break;
        }

        /* Generic fd-shape probes from the libc side.  ls(1)'s lstat()
         * lands here once the client has opened /dev/pci.  Reply with
         * a char-device stat so isatty(fd)==0 and ls prints "crw...". */
        case TM_REQ_FSTAT: {
            tm_stat_t *st = (tm_stat_t *)&rbuf[32];
            for (unsigned i = 0; i < sizeof *st; ++i)
                ((unsigned char *)st)[i] = 0;
            st->st_dev     = 9;
            st->st_ino     = 1;
            st->st_mode    = TM_S_IFCHR | 0666;
            st->st_nlink   = 1;
            st->st_rdev    = (9UL << 8) | 1;
            st->st_blksize = 256;
            unsigned want  = (unsigned)sizeof *st;
            for (int i = 0; i < 32; ++i) rbuf[i] = 0;
            rbuf[0] = (unsigned char)( want        & 0xff);
            rbuf[1] = (unsigned char)((want >>  8) & 0xff);
            rbuf[2] = (unsigned char)((want >> 16) & 0xff);
            rbuf[3] = (unsigned char)((want >> 24) & 0xff);
            MsgReply(rcvid, 0, rbuf, 32 + (int)sizeof *st);
            continue;
        }
        case TM_REQ_CLOSE:
            MsgReply(rcvid, 0, 0, 0);
            continue;

        default:
            break;
        }

        /* Common reply path: pack out_mr0/out_mr1 into rbuf bytes 0..15.   */
        for (int i = 0; i < 32; ++i) rbuf[i] = 0;
        pack_u64(rbuf, 0, (uint64_t)out_mr0);
        pack_u64(rbuf, 8, (uint64_t)out_mr1);
        MsgReply(rcvid, status,
                 rbuf, (int)(out_bytes ? out_bytes : 8));
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("[pci-server] alive, pid=%d\n", (int)qsoe_self_pid);
    fflush(stdout);

    if (hwi_init() != 0) {
        printf("[pci-server] hwi_init failed\n"); fflush(stdout);
        return 1;
    }
    if (ecam_init() != 0) {
        printf("[pci-server] ecam_init failed\n"); fflush(stdout);
        return 1;
    }
    scan_all();
    printf("[pci-server] scan complete: %u devices on bus 0\n", g_ndevs);
    fflush(stdout);

    int chid = ChannelCreate(0);
    if (chid < 0) {
        slogf(_SLOGC_PCI, _SLOG_ERROR, "pci-server: ChannelCreate failed");
        return 1;
    }
    if (qsoe_pathmgr_register(PCI_PATH, chid) != 0) {
        slogf(_SLOGC_PCI, _SLOG_ERROR, "pci-server: pathmgr_register(%s) failed",
              PCI_PATH);
        return 1;
    }

    slogf(_SLOGC_PCI, _SLOG_INFO, "pci-server: %s registered (chid=%d)",
          PCI_PATH, chid);

    /* Daemonise — init.sh's caller wakes once pci-server's path is
     * registered.  Mirrors slogger / ser8250 startup. */
    if (procmgr_detach(0) != 0) {
        slogf(_SLOGC_PCI, _SLOG_WARNING,
              "pci-server: procmgr_detach failed");
    }

    serve(chid);
    return 0;
}
