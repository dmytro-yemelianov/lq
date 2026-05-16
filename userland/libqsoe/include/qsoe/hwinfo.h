/*
 * qsoe/hwinfo.h — hardware-info accessors over the taskman syscfg blob.
 *
 * Wraps TM_REQ_GET_SYSCFG with a lazy, mutex-guarded per-process cache
 * so callers can probe tags repeatedly without paying the round-trip
 * cost every time.  The blob layout itself is documented in
 * <qsoe/syscfg.h>; this header just exposes a small cursor API that
 * matches the things ecam.c / pci-server need.
 *
 * Naming note: kept QNX/QRV-flavoured (hwi_*) so PCI-server source
 * ports across with minimal churn.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QSOE_HWINFO_H
#define QSOE_HWINFO_H

#include <qsoe-system.h>
#include <qsoe/syscfg.h>

/* Opaque cursor — a byte offset into the cached blob, or -1 on
 * not-found / error. */
typedef int qsoe_hwi_cursor_t;

/* Force the syscfg blob to be fetched if it hasn't been yet.  Returns
 * 0 on success, -1 if taskman has no blob (very early boot) or the
 * request fails.  Callers don't normally need to call this — every
 * hwi_* below auto-fetches on first use. */
int hwi_init(void);

/* Find the first tag with `tag_id`.  Returns the cursor or -1.        */
qsoe_hwi_cursor_t hwi_find_tag(unsigned tag_id);

/* Find the next tag with `tag_id` strictly after `prev`.  Returns -1
 * when there are no more matches. */
qsoe_hwi_cursor_t hwi_find_tag_after(qsoe_hwi_cursor_t prev,
                                     unsigned tag_id);

/* Get the payload byte count of the tag at `cursor`, or -1 if the
 * cursor is invalid. */
int hwi_tag_len(qsoe_hwi_cursor_t cursor);

/* Copy up to `cap` payload bytes from the tag at `cursor` into `out`.
 * Returns the payload length (NOT the bytes copied) on success, -1 on
 * invalid cursor. */
int hwi_tag_payload(qsoe_hwi_cursor_t cursor, void *out, unsigned cap);

/* Read a string-typed tag.  Returns a pointer into the cached blob
 * (valid for process lifetime) or 0 on error.  Use only for tags whose
 * payload is a NUL-terminated string (MODEL, COMPATIBLE). */
const char *hwi_tag_string(qsoe_hwi_cursor_t cursor);

/* Convenience: read a tag whose payload is a single u32. */
int hwi_read_u32(unsigned tag_id, uint32_t *out);

/* Convenience: read a tag whose payload is a single u64. */
int hwi_read_u64(unsigned tag_id, uint64_t *out);

#endif /* QSOE_HWINFO_H */
