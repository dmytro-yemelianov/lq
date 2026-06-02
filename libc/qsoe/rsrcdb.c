/*
 * rsrcdb.c — libqsoe client wrappers for the Resource Manager
 * Database.  Send rsrc_alloc_t / rsrc_request_t arrays to taskman
 * via TM_REQ_RSRC_*; copy the granted ranges (or query results)
 * back from msg[4..] into the caller's array.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>
#include <sys/rsrcdbmgr.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <qsoe/tls.h>

#include "sel4_types.h"
#include "qsoe_invoke.h"

/* Bytes available in the IPC payload area (msg[4..119] = 116 words). */
#define RSRC_PAYLOAD_BYTES   (116 * 8)

/* Copy `n` bytes between two buffers; minimal memcpy. */
static void blob_copy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; ++i) d[i] = s[i];
}

/* Common scaffold for the five operations.  Sends `count` entries of
 * `entry_size` bytes from `arr` to taskman, op `tm_req`.  On return,
 * if `copy_back`, copies the (possibly mutated) entries back from
 * the IPC buffer into `arr`.  Reply's mr0 is exposed via *out_mr0
 * for ops that report a count. */
static int rsrc_round_trip(unsigned tm_req, void *arr, unsigned count,
                           unsigned entry_size, int copy_back,
                           seL4_Word *out_mr0_unused,
                           seL4_Word extra_mr1, seL4_Word extra_mr2)
{
    unsigned long bytes = (unsigned long)count * entry_size;
    if (bytes > RSRC_PAYLOAD_BYTES) {
        qsoe_errno = E2BIG;
        return -1;
    }
    /* Stage the request in the IPC buffer. */
    blob_copy((void *)&qsoe_ipcbuf->msg[4], arr, bytes);

    seL4_Word mr0 = (seL4_Word)count;
    seL4_Word mr1 = extra_mr1;
    seL4_Word mr2 = extra_mr2;
    seL4_Word mr3 = 0;
    /* Length covers MR0..3 quad plus the payload words in msg[4..]. */
    unsigned nwords = 4 + (unsigned)((bytes + 7) / 8);
    seL4_MessageInfo_t tag = seL4_MessageInfo_new((seL4_Word)tm_req,
                                                   0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    if (copy_back) {
        blob_copy(arr, (const void *)&qsoe_ipcbuf->msg[4], bytes);
    }
    if (out_mr0_unused) *out_mr0_unused = mr0;
    return 0;
}

int rsrcdbmgr_create(rsrc_alloc_t *items, unsigned count)
{
    if (!items || count == 0) { qsoe_errno = EINVAL; return -1; }
    return rsrc_round_trip(TM_REQ_RSRC_CREATE, items, count,
                           sizeof(rsrc_alloc_t), 0, 0, 0, 0);
}

int rsrcdbmgr_destroy(rsrc_alloc_t *items, unsigned count)
{
    if (!items || count == 0) { qsoe_errno = EINVAL; return -1; }
    return rsrc_round_trip(TM_REQ_RSRC_DESTROY, items, count,
                           sizeof(rsrc_alloc_t), 0, 0, 0, 0);
}

int rsrcdbmgr_attach(rsrc_request_t *list, unsigned count)
{
    if (!list || count == 0) { qsoe_errno = EINVAL; return -1; }
    /* ATTACH echoes granted ranges back into the request array.    */
    return rsrc_round_trip(TM_REQ_RSRC_ATTACH, list, count,
                           sizeof(rsrc_request_t), 1, 0, 0, 0);
}

int rsrcdbmgr_detach(rsrc_request_t *list, unsigned count)
{
    if (!list || count == 0) { qsoe_errno = EINVAL; return -1; }
    return rsrc_round_trip(TM_REQ_RSRC_DETACH, list, count,
                           sizeof(rsrc_request_t), 0, 0, 0, 0);
}

int rsrcdbmgr_query(rsrc_alloc_t *list, int listcnt, int start,
                    uint32_t type)
{
    if (listcnt < 0 || start < 0) { qsoe_errno = EINVAL; return -1; }
    /* Query result is written into msg[4..]; copy out as many entries
     * as taskman tells us were written (capped at listcnt). */
    seL4_Word mr0 = (seL4_Word)listcnt;
    seL4_Word mr1 = (seL4_Word)start;
    seL4_Word mr2 = (seL4_Word)type;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_RSRC_QUERY,
                                                   0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    int written = (int)mr0;
    if (list && listcnt > 0 && written > 0) {
        unsigned long bytes = (unsigned long)written * sizeof(rsrc_alloc_t);
        if (bytes <= RSRC_PAYLOAD_BYTES) {
            blob_copy(list, (const void *)&qsoe_ipcbuf->msg[4], bytes);
        }
    }
    return written;
}
