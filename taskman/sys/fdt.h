/*
 * fdt.h — minimal device-tree blob parser for taskman.
 *
 * FDT is a binary format with a header at offset 0 (big-endian
 * fields), a "structure block" of token-tagged nodes/props, and a
 * "strings block" of null-terminated property names.  This parser
 * handles just the slice taskman needs at boot:
 *
 *   - validate header + walk
 *   - find a node by absolute path ("/cpus", "/soc/pci@30000000")
 *   - find first node whose `compatible` property contains a string
 *   - read u32 / u64 / string / raw-bytes properties
 *   - read `reg` (base, size) tuples accounting for
 *     parent-level #address-cells and #size-cells
 *
 * Returns small opaque "offset" handles (byte index into the
 * structure block).  Zero is a valid offset for the root node, so
 * lookup failures return -1.
 *
 * No allocation — the parser is stateless, every call takes the
 * blob pointer.  Thread-safe by construction.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_TASKMAN_FDT_H
#define QSOE_TASKMAN_FDT_H

#include <qsoe-system.h>     /* uint{16,32,64}_t */

/* Magic identifier at offset 0 of every FDT (stored big-endian). */
#define TM_FDT_MAGIC  0xd00dfeedU

/* Returns 0 if `blob` looks like a valid FDT, -1 otherwise. */
int tm_fdt_check(const void *blob);

/* Total size of the blob in bytes (from header.totalsize), or 0 if
 * the header doesn't validate. */
unsigned tm_fdt_size(const void *blob);

/* Find a node by absolute path.  Returns the structure-block offset
 * of the node's FDT_BEGIN_NODE token, or -1 if not found.  Root path
 * "/" returns the offset of the implicit root node. */
int tm_fdt_path(const void *blob, const char *path);

/* Walk every node looking for the first one whose `compatible`
 * property contains `compat` as one of its strings.  Returns the
 * node offset, or -1. */
int tm_fdt_compatible(const void *blob, const char *compat);

/* Get a raw property by name.  *out_ptr is set to a pointer into
 * `blob` (the property value); *out_len to its byte length.  Returns
 * 0 on success, -1 if the property doesn't exist on this node. */
int tm_fdt_prop(const void *blob, int node, const char *name,
                const void **out_ptr, unsigned *out_len);

/* Convenience: u32 property (big-endian on the wire, host-endian
 * out). */
int tm_fdt_prop_u32(const void *blob, int node, const char *name,
                    uint32_t *out);

/* Convenience: u64 property (big-endian on the wire). */
int tm_fdt_prop_u64(const void *blob, int node, const char *name,
                    uint64_t *out);

/* Convenience: string property.  *out_str points into the blob
 * (NUL-terminated).  Returns 0 on success, -1 otherwise. */
int tm_fdt_prop_str(const void *blob, int node, const char *name,
                    const char **out_str);

/* Read the i-th (base, size) tuple from this node's `reg` property,
 * using #address-cells and #size-cells from the parent (which the
 * caller passes — root has 2,1 by spec).  Returns 0 on success, -1
 * if i is out of range or the property is missing/malformed. */
int tm_fdt_reg(const void *blob, int node,
               unsigned addr_cells, unsigned size_cells,
               unsigned idx, uint64_t *out_base, uint64_t *out_size);

#endif /* QSOE_TASKMAN_FDT_H */
