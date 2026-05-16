/*
 * hwinfo.c — client-side wrapper over taskman's syscfg blob.
 *
 * On first use, fetches the whole blob via TM_REQ_GET_SYSCFG and
 * stores it in a per-process static buffer.  Subsequent calls walk
 * that buffer locally.  Refresh-on-change is not provided — the blob
 * is built once at taskman boot and is immutable, so a single cache
 * load is enough.
 *
 * Thread safety: the cache is fetched lazily; the first-to-call wins
 * and any concurrent racer redoes the same work harmlessly (writing
 * the same bytes into the same buffer).  No locking — adding a real
 * mutex is a v0.9 concern once libqsoe grows generic sync primitives.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>
#include <qsoe/syscfg.h>
#include <qsoe/hwinfo.h>
#include <qsoe/wire.h>
#include <qsoe/slots.h>

#include "sel4_types.h"
#include "sel4_syscalls.h"
#include "qsoe_invoke.h"

static unsigned char s_blob[TM_SYSCFG_MAX];
static unsigned      s_blob_len;
static int           s_blob_ready;

/* Pull the syscfg blob from taskman.  Returns 0 on success, -1 on
 * failure (errno set).  Idempotent. */
int hwi_init(void)
{
    if (s_blob_ready) return 0;

    seL4_Word mr0 = (seL4_Word)TM_SYSCFG_MAX;
    seL4_Word mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_GET_SYSCFG,
                                                   0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    unsigned got = (unsigned)mr0;
    if (got > TM_SYSCFG_MAX) got = TM_SYSCFG_MAX;
    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < got; ++i) s_blob[i] = src[i];
    s_blob_len = got;
    s_blob_ready = 1;
    return 0;
}

/* Walk the blob from `start` looking for `tag_id`.  Returns the offset
 * of the matching tag header, or -1. */
static int walk_from(unsigned start, unsigned tag_id)
{
    unsigned off = start;
    while (off + 4 <= s_blob_len) {
        unsigned id  = (unsigned)(s_blob[off] | (s_blob[off + 1] << 8));
        unsigned len = (unsigned)(s_blob[off + 2] | (s_blob[off + 3] << 8));
        if (id == TM_SYSCFG_TAG_END) return -1;
        if (id == tag_id) return (int)off;
        off += 4 + len;
    }
    return -1;
}

qsoe_hwi_cursor_t hwi_find_tag(unsigned tag_id)
{
    if (hwi_init() != 0) return -1;
    return (qsoe_hwi_cursor_t)walk_from(0, tag_id);
}

qsoe_hwi_cursor_t hwi_find_tag_after(qsoe_hwi_cursor_t prev,
                                     unsigned tag_id)
{
    if (hwi_init() != 0) return -1;
    if (prev < 0) return -1;
    /* Step past the current tag (header + payload). */
    unsigned off = (unsigned)prev;
    if (off + 4 > s_blob_len) return -1;
    unsigned len = (unsigned)(s_blob[off + 2] | (s_blob[off + 3] << 8));
    return (qsoe_hwi_cursor_t)walk_from(off + 4 + len, tag_id);
}

int hwi_tag_len(qsoe_hwi_cursor_t cursor)
{
    if (cursor < 0) return -1;
    unsigned off = (unsigned)cursor;
    if (off + 4 > s_blob_len) return -1;
    return (int)(s_blob[off + 2] | (s_blob[off + 3] << 8));
}

int hwi_tag_payload(qsoe_hwi_cursor_t cursor, void *out, unsigned cap)
{
    int len = hwi_tag_len(cursor);
    if (len < 0) return -1;
    if (out && cap > 0) {
        unsigned n = (unsigned)len < cap ? (unsigned)len : cap;
        const unsigned char *src = &s_blob[(unsigned)cursor + 4];
        unsigned char *dst = (unsigned char *)out;
        for (unsigned i = 0; i < n; ++i) dst[i] = src[i];
    }
    return len;
}

const char *hwi_tag_string(qsoe_hwi_cursor_t cursor)
{
    if (cursor < 0) return 0;
    unsigned off = (unsigned)cursor;
    int len = hwi_tag_len(cursor);
    if (len <= 0) return 0;
    /* Must end in NUL within len. */
    const char *s = (const char *)&s_blob[off + 4];
    for (int i = 0; i < len; ++i) {
        if (s[i] == 0) return s;
    }
    return 0;
}

int hwi_read_u32(unsigned tag_id, uint32_t *out)
{
    qsoe_hwi_cursor_t c = hwi_find_tag(tag_id);
    if (c < 0) return -1;
    if (hwi_tag_len(c) != 4) return -1;
    const unsigned char *bp = &s_blob[(unsigned)c + 4];
    *out = (uint32_t)bp[0] |
           ((uint32_t)bp[1] <<  8) |
           ((uint32_t)bp[2] << 16) |
           ((uint32_t)bp[3] << 24);
    return 0;
}

int hwi_read_u64(unsigned tag_id, uint64_t *out)
{
    qsoe_hwi_cursor_t c = hwi_find_tag(tag_id);
    if (c < 0) return -1;
    if (hwi_tag_len(c) != 8) return -1;
    const unsigned char *bp = &s_blob[(unsigned)c + 4];
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | bp[i];
    *out = v;
    return 0;
}
