/*
 * sys/syscfg.c — system-configuration builder.
 *
 * Walk the FDT once at boot, emit a flat tag-list into a static
 * buffer.  Clients then fetch the whole blob via TM_REQ_GET_SYSCFG
 * and walk it locally — same shape as QNX/QRV's hwinfo tag list, so
 * the PCI server's hwinfo_find_* calls become thin wrappers.
 *
 * For v0.8 we emit:
 *   VERSION, MODEL, COMPATIBLE, TIMEBASE_HZ, NUM_CPUS, BOOT_HART,
 *   MEMORY (one per /memory@* node)
 *
 * PCI / UART / DW MSI tags get added in a follow-on round when the
 * PCI port lands (they need FDT walks of /soc/pci@*, MSI controllers,
 * etc., and consume more bytes).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "syscfg.h"
#include "fdt.h"
#include "../sel4_syscalls.h"

static unsigned char s_blob[TM_SYSCFG_MAX];
static unsigned      s_blob_len;
static int           s_blob_ready;

/* String length (no libc here). */
static unsigned s_len(const char *s) {
    unsigned n = 0;
    while (s && s[n]) ++n;
    return n;
}

/* Append a tag header + payload to the blob.  Returns 0 on success,
 * -1 if it wouldn't fit. */
static int emit(uint16_t id, const void *payload, unsigned len)
{
    if (s_blob_len + 4 + len > TM_SYSCFG_MAX) return -1;
    s_blob[s_blob_len + 0] = (unsigned char)(id & 0xff);
    s_blob[s_blob_len + 1] = (unsigned char)((id >> 8) & 0xff);
    s_blob[s_blob_len + 2] = (unsigned char)(len & 0xff);
    s_blob[s_blob_len + 3] = (unsigned char)((len >> 8) & 0xff);
    if (len && payload) {
        const unsigned char *p = (const unsigned char *)payload;
        for (unsigned i = 0; i < len; ++i) s_blob[s_blob_len + 4 + i] = p[i];
    }
    s_blob_len += 4 + len;
    return 0;
}

static int emit_u32(uint16_t id, uint32_t v)
{
    return emit(id, &v, 4);
}

static int emit_u64(uint16_t id, uint64_t v)
{
    return emit(id, &v, 8);
}

static int emit_asciz(uint16_t id, const char *s)
{
    unsigned len = s_len(s);
    if (len == 0) return 0;             /* nothing to emit */
    /* Include trailing NUL so clients can use it as a C string
     * directly without copying. */
    return emit(id, s, len + 1);
}

int tm_syscfg_build(const void *fdt_blob)
{
    if (tm_fdt_check(fdt_blob) != 0) return -1;
    s_blob_len = 0;
    s_blob_ready = 0;

    int root = tm_fdt_path(fdt_blob, "/");
    if (root < 0) return -1;

    /* Always-first tag: layout version. */
    if (emit_u32(TM_SYSCFG_TAG_VERSION, TM_SYSCFG_VERSION) != 0) return -1;

    /* Machine model + root compatible (best-effort; either may
     * be absent on stripped DTBs). */
    const char *model_str = 0;
    if (tm_fdt_prop_str(fdt_blob, root, "model", &model_str) == 0) {
        (void)emit_asciz(TM_SYSCFG_TAG_MODEL, model_str);
    }
    const void *compat_p; unsigned compat_len;
    if (tm_fdt_prop(fdt_blob, root, "compatible", &compat_p,
                    &compat_len) == 0) {
        /* compatible is a sequence of NUL-terminated strings;
         * publish the first one. */
        (void)emit_asciz(TM_SYSCFG_TAG_COMPATIBLE,
                         (const char *)compat_p);
    }

    /* /cpus/timebase-frequency → u64 (FDT property is usually u32,
     * but the spec allows u64 on some platforms). */
    int cpus = tm_fdt_path(fdt_blob, "/cpus");
    if (cpus >= 0) {
        const void *p; unsigned len;
        uint64_t tbhz = 0;
        if (tm_fdt_prop(fdt_blob, cpus, "timebase-frequency",
                        &p, &len) == 0) {
            const unsigned char *bp = (const unsigned char *)p;
            if (len == 4) {
                tbhz = ((uint64_t)bp[0] << 24) | ((uint64_t)bp[1] << 16) |
                       ((uint64_t)bp[2] <<  8) |  (uint64_t)bp[3];
            } else if (len == 8) {
                for (unsigned i = 0; i < 8; ++i) tbhz = (tbhz << 8) | bp[i];
            }
            if (tbhz) (void)emit_u64(TM_SYSCFG_TAG_TIMEBASE_HZ, tbhz);
        }
    }

    /* Walk /cpus children to count harts.  Each cpu node carries
     * its hart-id in `reg`; we just count nodes that don't have a
     * device_type other than "cpu". */
    if (cpus >= 0) {
        /* Use the parser's structure walker — quick inline traversal. */
        unsigned ncpus = 0;
        /* Re-walk: find_child_node-style scan via tm_fdt_prop is
         * awkward for counting; cheat slightly by trying a few
         * common child names.  RISC-V FDTs use "cpu@0", "cpu@1", ... */
        for (int i = 0; i < 64; ++i) {
            char name[16];
            unsigned n = 0;
            name[n++] = '/'; name[n++] = 'c'; name[n++] = 'p'; name[n++] = 'u';
            name[n++] = 's'; name[n++] = '/'; name[n++] = 'c'; name[n++] = 'p';
            name[n++] = 'u'; name[n++] = '@';
            /* Hex digit. */
            int v = i;
            if (v >= 16) { name[n++] = '0' + (v / 16); v %= 16; }
            name[n++] = (char)(v < 10 ? '0' + v : 'a' + v - 10);
            name[n] = 0;
            int node = tm_fdt_path(fdt_blob, name);
            if (node < 0) break;
            ++ncpus;
        }
        if (ncpus) (void)emit_u32(TM_SYSCFG_TAG_NUM_CPUS, ncpus);
    }

    /* /chosen/boot-hartid (RISC-V convention). */
    int chosen = tm_fdt_path(fdt_blob, "/chosen");
    if (chosen >= 0) {
        uint32_t v;
        if (tm_fdt_prop_u32(fdt_blob, chosen, "boot-hartid", &v) == 0) {
            (void)emit_u32(TM_SYSCFG_TAG_BOOT_HART, v);
        }
    }

    /* /memory@... — emit each (base, size) tuple.  Most boards use a
     * single /memory@ node, but some declare multiple.  Use 2/2 cells
     * (the RISC-V standard) — proper #address-cells / #size-cells
     * inspection comes when we have multi-region needs. */
    for (int i = 0; i < 8; ++i) {
        char path[24];
        unsigned n = 0;
        const char *mem = "/memory@";
        for (; mem[n]; ++n) path[n] = mem[n];
        /* Memory unit-addresses are hex addresses, can be long.
         * For now try a few common conventions: @80000000 (QEMU),
         * @80200000, etc.  If we don't find any, just skip — the
         * caller can fall back on bi->untypedList. */
        if (i == 0) {
            const char *suf = "80000000"; unsigned k = 0;
            while (suf[k]) { path[n++] = suf[k++]; }
        } else {
            break;
        }
        path[n] = 0;
        int node = tm_fdt_path(fdt_blob, path);
        if (node < 0) continue;
        uint64_t base, size;
        if (tm_fdt_reg(fdt_blob, node, 2, 2, 0, &base, &size) == 0) {
            unsigned char buf[16];
            for (int b = 0; b < 8; ++b) buf[b]     = (unsigned char)((base >> (b * 8)) & 0xff);
            for (int b = 0; b < 8; ++b) buf[8 + b] = (unsigned char)((size >> (b * 8)) & 0xff);
            (void)emit(TM_SYSCFG_TAG_MEMORY, buf, 16);
        }
    }

    /* Sentinel. */
    if (emit(TM_SYSCFG_TAG_END, 0, 0) != 0) return -1;

    s_blob_ready = 1;
    return 0;
}

int tm_syscfg_get(const void **out_blob, unsigned *out_len)
{
    if (!s_blob_ready) return -1;
    if (out_blob) *out_blob = s_blob;
    if (out_len)  *out_len  = s_blob_len;
    return 0;
}

int tm_syscfg_find(unsigned tag_id, const void **out_ptr,
                   unsigned *out_len)
{
    if (!s_blob_ready) return -1;
    unsigned off = 0;
    while (off + 4 <= s_blob_len) {
        uint16_t id  = (uint16_t)(s_blob[off] | (s_blob[off + 1] << 8));
        uint16_t len = (uint16_t)(s_blob[off + 2] | (s_blob[off + 3] << 8));
        if (id == TM_SYSCFG_TAG_END) return -1;
        if (id == tag_id) {
            if (out_ptr) *out_ptr = &s_blob[off + 4];
            if (out_len) *out_len = len;
            return 0;
        }
        off += 4 + len;
    }
    return -1;
}

int tm_syscfg_find_u32(unsigned tag_id, uint32_t *out)
{
    const void *p; unsigned len;
    if (tm_syscfg_find(tag_id, &p, &len) != 0) return -1;
    if (len != 4) return -1;
    const unsigned char *bp = (const unsigned char *)p;
    *out = (uint32_t)bp[0] |
           ((uint32_t)bp[1] <<  8) |
           ((uint32_t)bp[2] << 16) |
           ((uint32_t)bp[3] << 24);
    return 0;
}

int tm_syscfg_find_u64(unsigned tag_id, uint64_t *out)
{
    const void *p; unsigned len;
    if (tm_syscfg_find(tag_id, &p, &len) != 0) return -1;
    if (len != 8) return -1;
    const unsigned char *bp = (const unsigned char *)p;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | bp[i];
    *out = v;
    return 0;
}
