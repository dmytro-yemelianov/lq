/*
 * sys/sysmap.c -- build the QSOE sysmap page for LQ.
 *
 * On NQ the Skimmer kernel builds the sysmap and maps it in the boot
 * page table; seL4 has no such hook, so LQ's taskman builds the same
 * 'PSYS' TLV page here -- from the data tm_syscfg already parsed out of
 * the FDT -- and proc/spawn.c maps it read-only at QSOE_SYSMAP_VA into
 * every process it spawns.  The shared libc hwi_init() then reads it
 * with no IPC, identical to NQ.
 *
 * Only the tags the shared client consumes are emitted: MTIME_FREQ,
 * PLIC (s_context_count), and PCI_ECAM (ecam window + the 32-bit MMIO
 * window the pci-server carves BARs from).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe/sysmap.h>
#include "syscfg.h"
#include "sysmap.h"

/* The page taskman copies into each child's QSOE_SYSMAP_VA frame. */
#define TM_SYSMAP_PAGE_BYTES 0x1000U

static unsigned char s_page[TM_SYSMAP_PAGE_BYTES];
static unsigned      s_len;
static int           s_ready;

/* Read a little-endian u64 / u32 from a syscfg payload at byte `off`. */
static uint64_t rd64(const unsigned char *b, unsigned off)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[off + (unsigned)i];
    return v;
}
static uint32_t rd32(const unsigned char *b, unsigned off)
{
    return (uint32_t)b[off]            | ((uint32_t)b[off + 1] <<  8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}

/* Append one TLV record (tag, len, body), padded to the 8-byte stride
 * the qsoe_sysmap_find() walker advances by.  Silently drops a record
 * that would overflow the page (we emit far less than a page). */
static void emit(unsigned tag, const void *body, unsigned len)
{
    unsigned rec = 4 + len;
    unsigned pad = (rec + 7u) & ~7u;
    if (s_len + pad > sizeof s_page) return;
    s_page[s_len + 0] = (unsigned char)(tag & 0xff);
    s_page[s_len + 1] = (unsigned char)((tag >> 8) & 0xff);
    s_page[s_len + 2] = (unsigned char)(len & 0xff);
    s_page[s_len + 3] = (unsigned char)((len >> 8) & 0xff);
    const unsigned char *p = (const unsigned char *)body;
    for (unsigned i = 0; i < len; ++i) s_page[s_len + 4 + i] = p[i];
    for (unsigned i = rec; i < pad; ++i) s_page[s_len + i] = 0;
    s_len += pad;
}

int tm_sysmap_build(void)
{
    s_ready = 0;
    s_len   = 0;
    for (unsigned i = 0; i < sizeof s_page; ++i) s_page[i] = 0;

    /* Header (total_bytes patched once records are laid down). */
    qsoe_sysmap_hdr_t hdr;
    for (unsigned i = 0; i < sizeof hdr; ++i) ((unsigned char *)&hdr)[i] = 0;
    hdr.magic     = QSOE_SYSMAP_MAGIC;
    hdr.version   = QSOE_SYSMAP_VERSION;
    hdr.hdr_bytes = (uint16_t)sizeof hdr;
    for (unsigned i = 0; i < sizeof hdr; ++i) s_page[i] = ((unsigned char *)&hdr)[i];
    s_len = (unsigned)sizeof hdr;

    /* MTIME_FREQ <- syscfg TIMEBASE_HZ. */
    uint64_t hz = 0;
    if (tm_syscfg_find_u64(TM_SYSCFG_TAG_TIMEBASE_HZ, &hz) == 0 && hz) {
        qsoe_sysmap_mtime_freq_t m;
        m.hz = (uint32_t)hz;
        m._pad = 0;
        emit(QSOE_SYSMAP_TAG_MTIME_FREQ, &m, (unsigned)sizeof m);
    }

    /* PLIC: the shared hwi_init() consumes only s_context_count (which
     * it reports as NUM_CPUS).  Base/size/sources stay zero -- LQ's
     * interrupt path doesn't read them from the sysmap. */
    uint32_t ncpu = 0;
    if (tm_syscfg_find_u32(TM_SYSCFG_TAG_NUM_CPUS, &ncpu) == 0 && ncpu) {
        qsoe_sysmap_plic_t p;
        for (unsigned i = 0; i < sizeof p; ++i) ((unsigned char *)&p)[i] = 0;
        p.s_context_count = ncpu;
        emit(QSOE_SYSMAP_TAG_PLIC, &p, (unsigned)sizeof p);
    }

    /* PCI_ECAM: ecam base/size/lastbus from syscfg PCI_ECAM, plus the
     * 32-bit non-prefetchable MMIO window from the matching PCI_WINDOW
     * so the pci-server can assign device BARs.  dbi/msi_irq stay zero
     * -- flat ECAM on QEMU virt. */
    const void *ep;
    unsigned    el;
    if (tm_syscfg_find(TM_SYSCFG_TAG_PCI_ECAM, &ep, &el) == 0 && el >= 20) {
        const unsigned char *b = (const unsigned char *)ep;
        qsoe_sysmap_pci_ecam_t pe;
        for (unsigned i = 0; i < sizeof pe; ++i) ((unsigned char *)&pe)[i] = 0;
        pe.ecam_base = rd64(b, 0);
        pe.ecam_size = rd64(b, 8);
        pe.bus_start = 0;
        pe.bus_end   = (uint8_t)rd32(b, 16);

        /* Walk every PCI_WINDOW for a MEM, non-prefetchable entry. */
        const void *blob;
        unsigned    blen;
        if (tm_syscfg_get(&blob, &blen) == 0) {
            const unsigned char *bp = (const unsigned char *)blob;
            unsigned off = 0;
            while (off + 4 <= blen) {
                unsigned id  = (unsigned)bp[off] | ((unsigned)bp[off + 1] << 8);
                unsigned len = (unsigned)bp[off + 2] | ((unsigned)bp[off + 3] << 8);
                if (id == TM_SYSCFG_TAG_END) break;
                if (id == TM_SYSCFG_TAG_PCI_WINDOW && len >= 28) {
                    const unsigned char *w = bp + off + 4;
                    uint32_t flags = rd32(w, 24);
                    if ((flags & TM_SYSCFG_PCI_WINDOW_MEM) &&
                        !(flags & TM_SYSCFG_PCI_WINDOW_PREFETCH)) {
                        pe.mmio_cpu_base = rd64(w, 0);
                        pe.mmio_pci_base = rd64(w, 8);
                        pe.mmio_size     = rd64(w, 16);
                        break;
                    }
                }
                off += 4 + len;
            }
        }
        emit(QSOE_SYSMAP_TAG_PCI_ECAM, &pe, (unsigned)sizeof pe);
    }

    emit(QSOE_SYSMAP_TAG_END, 0, 0);

    /* Patch total_bytes now that the record region is laid down. */
    hdr.total_bytes = s_len;
    for (unsigned i = 0; i < sizeof hdr; ++i) s_page[i] = ((unsigned char *)&hdr)[i];

    s_ready = 1;
    return 0;
}

int tm_sysmap_get(const void **out_page, unsigned *out_len)
{
    if (!s_ready) return -1;
    if (out_page) *out_page = s_page;
    if (out_len)  *out_len  = (unsigned)sizeof s_page;
    return 0;
}
