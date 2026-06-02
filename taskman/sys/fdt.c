/*
 * fdt.c — minimal device-tree blob parser.  See fdt.h for the API.
 *
 * Layout of an FDT blob (all multi-byte integers big-endian):
 *
 *   header (40 bytes)
 *   memory reservation block        — pairs of (uint64, uint64),
 *                                     terminated by (0, 0)
 *   structure block                 — token stream (FDT_*) describing
 *                                     the node/property tree
 *   strings block                   — null-terminated property names,
 *                                     referenced by offset from PROPs
 *
 * Token format in the structure block (each token is u32 BE):
 *   FDT_BEGIN_NODE  (1)  followed by null-terminated node name,
 *                        then padded to a 4-byte boundary.
 *   FDT_END_NODE    (2)  no payload.
 *   FDT_PROP        (3)  followed by (len, name_off) u32s, then
 *                        `len` bytes of value, padded to 4 bytes.
 *   FDT_NOP         (4)  no payload; skip.
 *   FDT_END         (9)  end of structure block.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "fdt.h"

#define FDT_BEGIN_NODE  1
#define FDT_END_NODE    2
#define FDT_PROP        3
#define FDT_NOP         4
#define FDT_END         9

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static inline uint32_t be32(uint32_t v)
{
    return ((v & 0xffu) << 24) | ((v & 0xff00u) << 8) |
           ((v & 0xff0000u) >> 8) | ((v & 0xff000000u) >> 24);
}

static inline uint64_t be64(uint64_t v)
{
    return ((uint64_t)be32((uint32_t)v) << 32) | be32((uint32_t)(v >> 32));
}

static inline const uint32_t *fdt_struct_base(const void *blob)
{
    const struct fdt_header *h = (const struct fdt_header *)blob;
    return (const uint32_t *)((const char *)blob + be32(h->off_dt_struct));
}

static inline const char *fdt_strings_base(const void *blob)
{
    const struct fdt_header *h = (const struct fdt_header *)blob;
    return (const char *)blob + be32(h->off_dt_strings);
}

int tm_fdt_check(const void *blob)
{
    if (!blob) return -1;
    const struct fdt_header *h = (const struct fdt_header *)blob;
    if (be32(h->magic) != TM_FDT_MAGIC) return -1;
    if (be32(h->last_comp_version) > 17) return -1;
    return 0;
}

unsigned tm_fdt_size(const void *blob)
{
    if (tm_fdt_check(blob) != 0) return 0;
    const struct fdt_header *h = (const struct fdt_header *)blob;
    return be32(h->totalsize);
}

/* String compare; returns 0 if equal. */
static int s_eq(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 1; ++a; ++b; }
    return *a != *b;
}

/* Compare up to `n` bytes (stop early on null on either side). */
static int s_eqn(const char *a, const char *b, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        if (a[i] != b[i]) return 1;
        if (a[i] == 0) return 0;
    }
    return 0;
}

/* Length of null-terminated string. */
static unsigned s_len(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

/* Round x up to a multiple of 4. */
static inline unsigned align4(unsigned x) { return (x + 3u) & ~3u; }

/* Step the structure-block cursor past whatever token is at offset
 * `off`.  Returns the next offset (in bytes from struct-block base),
 * or -1 if the token is malformed / we hit FDT_END.  If *out_tok is
 * non-null, it's set to the token we just stepped over. */
static int step_token(const void *blob, int off, int *out_tok)
{
    const uint32_t *base = fdt_struct_base(blob);
    uint32_t tok = be32(base[off / 4]);
    if (out_tok) *out_tok = (int)tok;
    switch (tok) {
    case FDT_BEGIN_NODE: {
        const char *name = (const char *)base + off + 4;
        unsigned nlen = s_len(name) + 1;     /* include NUL */
        return off + 4 + (int)align4(nlen);
    }
    case FDT_END_NODE:
    case FDT_NOP:
        return off + 4;
    case FDT_PROP: {
        uint32_t plen = be32(base[(off + 4) / 4]);
        return off + 12 + (int)align4(plen);
    }
    case FDT_END:
    default:
        return -1;
    }
}

/* Walk forward from `off` until we hit the matching BEGIN_NODE for
 * the named component `comp`/`comp_len` at depth 0 (immediate child).
 * Returns the offset of the BEGIN_NODE token, or -1 if no match. */
static int find_child_node(const void *blob, int parent_off,
                           const char *comp, unsigned comp_len)
{
    const uint32_t *base = fdt_struct_base(blob);
    /* First step over the parent's BEGIN_NODE + name to land at its
     * body.  parent_off itself points at the parent's BEGIN_NODE. */
    int off = step_token(blob, parent_off, 0);
    int depth = 1;
    while (off >= 0) {
        int tok;
        switch (be32(base[off / 4])) {
        case FDT_BEGIN_NODE: {
            if (depth == 1) {
                const char *name = (const char *)base + off + 4;
                /* Match either "name" or "name@unit-addr" — the
                 * caller can pass either the bare name or the full
                 * thing.  s_eqn handles both: if comp is short
                 * (just "cpus") we still match "cpus@0" if the FDT
                 * has unit addresses.  But to be precise, only
                 * accept comp == name OR (s_eqn-match AND name[len]
                 * == '@'). */
                if ((s_eqn(name, comp, comp_len) == 0) &&
                    (name[comp_len] == 0 || name[comp_len] == '@')) {
                    return off;
                }
            }
            ++depth;
            off = step_token(blob, off, &tok);
            break;
        }
        case FDT_END_NODE:
            --depth;
            if (depth == 0) return -1;
            off = step_token(blob, off, &tok);
            break;
        case FDT_PROP:
        case FDT_NOP:
            off = step_token(blob, off, &tok);
            break;
        case FDT_END:
        default:
            return -1;
        }
    }
    return -1;
}

int tm_fdt_path(const void *blob, const char *path)
{
    if (tm_fdt_check(blob) != 0) return -1;
    if (!path || path[0] != '/') return -1;

    /* The implicit root node is the first BEGIN_NODE in the structure
     * block (its name is an empty string). */
    const uint32_t *base = fdt_struct_base(blob);
    int off = 0;
    while (be32(base[off / 4]) == FDT_NOP) off += 4;
    if (be32(base[off / 4]) != FDT_BEGIN_NODE) return -1;
    int node = off;

    /* "/" returns the root. */
    const char *p = path + 1;
    if (*p == 0) return node;

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') ++p;
        unsigned len = (unsigned)(p - start);
        node = find_child_node(blob, node, start, len);
        if (node < 0) return -1;
        if (*p == '/') ++p;
    }
    return node;
}

int tm_fdt_prop(const void *blob, int node, const char *name,
                const void **out_ptr, unsigned *out_len)
{
    if (tm_fdt_check(blob) != 0 || node < 0) return -1;
    const uint32_t *base = fdt_struct_base(blob);
    const char *strings = fdt_strings_base(blob);

    /* Step past the node's BEGIN_NODE + name to the body. */
    int off = step_token(blob, node, 0);
    int depth = 1;
    while (off >= 0) {
        uint32_t tok = be32(base[off / 4]);
        if (tok == FDT_PROP && depth == 1) {
            uint32_t plen     = be32(base[(off + 4) / 4]);
            uint32_t nameoff  = be32(base[(off + 8) / 4]);
            const char *pname = strings + nameoff;
            if (s_eq(pname, name) == 0) {
                if (out_ptr) *out_ptr = (const char *)base + off + 12;
                if (out_len) *out_len = plen;
                return 0;
            }
            off = step_token(blob, off, 0);
        } else if (tok == FDT_BEGIN_NODE) {
            ++depth;
            off = step_token(blob, off, 0);
        } else if (tok == FDT_END_NODE) {
            --depth;
            if (depth == 0) return -1;     /* end of our node */
            off = step_token(blob, off, 0);
        } else if (tok == FDT_NOP) {
            off += 4;
        } else {
            return -1;
        }
    }
    return -1;
}

int tm_fdt_prop_u32(const void *blob, int node, const char *name,
                    uint32_t *out)
{
    const void *p; unsigned len;
    if (tm_fdt_prop(blob, node, name, &p, &len) != 0) return -1;
    if (len != 4) return -1;
    uint32_t v;
    const unsigned char *bp = (const unsigned char *)p;
    v = ((uint32_t)bp[0] << 24) | ((uint32_t)bp[1] << 16) |
        ((uint32_t)bp[2] <<  8) |  (uint32_t)bp[3];
    *out = v;
    return 0;
}

int tm_fdt_prop_u64(const void *blob, int node, const char *name,
                    uint64_t *out)
{
    const void *p; unsigned len;
    if (tm_fdt_prop(blob, node, name, &p, &len) != 0) return -1;
    if (len != 8) return -1;
    const unsigned char *bp = (const unsigned char *)p;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | bp[i];
    *out = v;
    return 0;
}

int tm_fdt_prop_str(const void *blob, int node, const char *name,
                    const char **out_str)
{
    const void *p; unsigned len;
    if (tm_fdt_prop(blob, node, name, &p, &len) != 0) return -1;
    /* Validate that the property is NUL-terminated within `len`. */
    const char *s = (const char *)p;
    for (unsigned i = 0; i < len; ++i) if (s[i] == 0) { *out_str = s; return 0; }
    return -1;
}

/* Walk every node depth-first; for each, look for a `compatible`
 * property and check if any of its strings match `compat`.  Returns
 * the BEGIN_NODE offset of the first match. */
int tm_fdt_compatible(const void *blob, const char *compat)
{
    if (tm_fdt_check(blob) != 0 || !compat) return -1;
    const uint32_t *base = fdt_struct_base(blob);

    int off = 0;
    while (be32(base[off / 4]) == FDT_NOP) off += 4;
    if (be32(base[off / 4]) != FDT_BEGIN_NODE) return -1;

    int depth = 0;
    while (off >= 0) {
        uint32_t tok = be32(base[off / 4]);
        if (tok == FDT_BEGIN_NODE) {
            int node = off;
            ++depth;
            /* Probe this node's compatible — if a match, return. */
            const void *p; unsigned len;
            if (tm_fdt_prop(blob, node, "compatible", &p, &len) == 0) {
                const char *s = (const char *)p;
                unsigned i = 0;
                while (i < len) {
                    if (s_eq(s + i, compat) == 0) return node;
                    while (i < len && s[i] != 0) ++i;
                    ++i; /* skip NUL */
                }
            }
            off = step_token(blob, off, 0);
        } else if (tok == FDT_END_NODE) {
            --depth;
            off = step_token(blob, off, 0);
            if (depth == 0) return -1;
        } else if (tok == FDT_PROP) {
            off = step_token(blob, off, 0);
        } else if (tok == FDT_NOP) {
            off += 4;
        } else {
            return -1;
        }
    }
    return -1;
}

int tm_fdt_reg(const void *blob, int node,
               unsigned addr_cells, unsigned size_cells,
               unsigned idx, uint64_t *out_base, uint64_t *out_size)
{
    const void *p; unsigned len;
    if (tm_fdt_prop(blob, node, "reg", &p, &len) != 0) return -1;

    unsigned tuple_words = addr_cells + size_cells;
    if (tuple_words == 0) return -1;
    unsigned tuple_bytes = tuple_words * 4;
    if ((idx + 1) * tuple_bytes > len) return -1;

    const unsigned char *bp = (const unsigned char *)p + idx * tuple_bytes;

    uint64_t base = 0;
    for (unsigned i = 0; i < addr_cells * 4; ++i) {
        base = (base << 8) | bp[i];
    }
    uint64_t size = 0;
    bp += addr_cells * 4;
    for (unsigned i = 0; i < size_cells * 4; ++i) {
        size = (size << 8) | bp[i];
    }

    if (out_base) *out_base = base;
    if (out_size) *out_size = size;
    return 0;
}
