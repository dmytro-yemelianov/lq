/*
 * sys/rsrcdb.c — Resource Manager Database, taskman side.
 *
 * Storage: a fixed pool of `tm_rsrc_t` entries (no malloc inside
 * taskman) plus a freelist.  Per-class sorted singly-linked list,
 * each entry covers [start, end) with owner == 0 meaning FREE.
 *
 * Allocation algorithm (rsrc_attach):
 *   For each rsrc_request_t in the batch:
 *     Walk the class list looking for a FREE entry that fits.  The
 *     fit constraints depend on flags:
 *       FLAG_RANGE  — granted range must lie within [req.start,req.end]
 *       FLAG_ALIGN  — granted start must be req.align-aligned
 *       length>0    — granted size must be at least req.length
 *     If a fit is found, split the FREE entry around the granted
 *     [g_start, g_end] and mark the carved entry as USED + owner.
 *     Echo the granted range back into the request (start/end).
 *   On any miss, roll back previous successes and reply with
 *   `ENOSPC`.
 *
 * Deallocation merges with adjacent FREE entries of the same class.
 *
 * For v0.8-rc1: linear walk per class; 256-entry pool; no name-based
 * lookup yet (FLAG_NAME ignored except as a passthrough field).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rsrcdb.h"
#include "syscfg.h"
#include "../sel4_syscalls.h"
#include "../sel4_types.h"
#include "../qsoe_invoke.h"

#define TM_RSRC_POOL_SIZE   256
#define TM_RSRC_NAME_MAX    28

typedef struct tm_rsrc {
    struct tm_rsrc *next;          /* sorted-by-start within class    */
    uint64_t        start;
    uint64_t        end;           /* inclusive — matches QRV         */
    pid_t           owner;         /* 0 = FREE                        */
    uint16_t        class;         /* RSRCDBMGR_MEMORY etc.           */
    uint16_t        flags;         /* USED / SHARE / NAME etc.        */
    char            name[TM_RSRC_NAME_MAX];
} tm_rsrc_t;

static tm_rsrc_t   s_pool[TM_RSRC_POOL_SIZE];
static tm_rsrc_t  *s_free_list;
static tm_rsrc_t  *s_class_head[RSRCDBMGR_TYPE_COUNT];

/* ---------- pool helpers ----------------------------------------- */

static void pool_init(void)
{
    s_free_list = 0;
    for (int i = TM_RSRC_POOL_SIZE - 1; i >= 0; --i) {
        s_pool[i].next = s_free_list;
        s_free_list    = &s_pool[i];
    }
    for (unsigned c = 0; c < RSRCDBMGR_TYPE_COUNT; ++c) {
        s_class_head[c] = 0;
    }
}

static tm_rsrc_t *pool_alloc(void)
{
    tm_rsrc_t *e = s_free_list;
    if (!e) return 0;
    s_free_list = e->next;
    e->next = 0; e->start = 0; e->end = 0;
    e->owner = 0; e->class = 0; e->flags = 0;
    for (unsigned i = 0; i < TM_RSRC_NAME_MAX; ++i) e->name[i] = 0;
    return e;
}

static void pool_free(tm_rsrc_t *e)
{
    e->next = s_free_list;
    s_free_list = e;
}

/* ---------- class-list helpers ----------------------------------- */

/* Insert e into the class list at the position that keeps the list
 * sorted by start.  Caller has already set e->class / start / end. */
static void list_insert(tm_rsrc_t *e)
{
    if (e->class >= RSRCDBMGR_TYPE_COUNT) return;
    tm_rsrc_t **pp = &s_class_head[e->class];
    while (*pp && (*pp)->start < e->start) pp = &(*pp)->next;
    e->next = *pp;
    *pp = e;
}

static void list_remove(tm_rsrc_t *e)
{
    if (e->class >= RSRCDBMGR_TYPE_COUNT) return;
    tm_rsrc_t **pp = &s_class_head[e->class];
    while (*pp && *pp != e) pp = &(*pp)->next;
    if (*pp) *pp = e->next;
    e->next = 0;
}

/* Best-fit-from-start walk: find a FREE entry of class `class` that
 * fits the request.  Returns the entry (and the granted [start,end]
 * within it via the output args), or 0 on miss.                    */
static tm_rsrc_t *find_fit(uint16_t class, const struct rsrc_request *req,
                            uint64_t *out_start, uint64_t *out_end)
{
    if (class >= RSRCDBMGR_TYPE_COUNT) return 0;
    int range  = (req->flags & RSRCDBMGR_FLAG_RANGE)  != 0;
    int aligned = (req->flags & RSRCDBMGR_FLAG_ALIGN) != 0;
    uint64_t align = aligned ? req->align : 1;
    if (align == 0) align = 1;

    for (tm_rsrc_t *e = s_class_head[class]; e; e = e->next) {
        if (e->owner != 0) continue;          /* skip USED        */
        uint64_t lo = e->start;
        uint64_t hi = e->end;
        if (range) {
            if (req->start > lo) lo = req->start;
            if (req->end   < hi) hi = req->end;
        }
        if (hi < lo) continue;
        /* Align up. */
        if (align > 1) lo = (lo + align - 1) & ~(align - 1);
        uint64_t granted_end = lo + (req->length ? req->length - 1 : 0);
        if (granted_end < lo) continue;       /* overflow         */
        if (granted_end > hi) continue;       /* doesn't fit      */
        *out_start = lo;
        *out_end   = granted_end;
        return e;
    }
    return 0;
}

/* Split FREE entry `e` so that [g_start, g_end] becomes a fresh USED
 * entry (owner = caller).  May leave up to two FREE residual entries
 * (left and right of the carve).  Returns 0 on success, -ENOMEM on
 * pool exhaustion (no leak — caller's request just fails).         */
static int carve(tm_rsrc_t *e, uint64_t g_start, uint64_t g_end,
                 pid_t owner, uint32_t flags, const char *name)
{
    /* Left residual (if any) reuses e itself — easy. */
    /* Right residual needs a fresh entry. */
    tm_rsrc_t *carved = pool_alloc();
    if (!carved) return -ENOMEM;
    tm_rsrc_t *right = 0;
    if (g_end < e->end) {
        right = pool_alloc();
        if (!right) { pool_free(carved); return -ENOMEM; }
    }

    carved->class = e->class;
    carved->start = g_start;
    carved->end   = g_end;
    carved->owner = owner;
    carved->flags = (uint16_t)((flags & RSRCDBMGR_FLAG_MASK) >> 8);
    if (name) {
        for (unsigned i = 0; i + 1 < TM_RSRC_NAME_MAX && name[i]; ++i) {
            carved->name[i] = name[i];
        }
    }

    /* Right residual = [g_end+1 .. e->end] FREE */
    if (right) {
        right->class = e->class;
        right->start = g_end + 1;
        right->end   = e->end;
        right->owner = 0;
        right->flags = 0;
    }

    /* Shrink `e` to the LEFT residual.  If g_start == e->start the
     * left residual is empty — remove e entirely. */
    if (g_start > e->start) {
        e->end = g_start - 1;     /* left residual remains FREE     */
    } else {
        list_remove(e);
        pool_free(e);
    }

    list_insert(carved);
    if (right) list_insert(right);
    return 0;
}

/* Merge FREE entry `e` with adjacent FREE entries of the same class.
 * Called after a DETACH transitions an entry FREE.                   */
static void merge_neighbours(tm_rsrc_t *e)
{
    /* Find the predecessor in the class list (linear). */
    tm_rsrc_t *prev = 0;
    for (tm_rsrc_t *p = s_class_head[e->class]; p && p != e; p = p->next) {
        prev = p;
    }
    /* Merge with right neighbour. */
    if (e->next && e->next->owner == 0 && e->next->start == e->end + 1) {
        tm_rsrc_t *r = e->next;
        e->end  = r->end;
        e->next = r->next;
        pool_free(r);
    }
    /* Merge with left neighbour. */
    if (prev && prev->owner == 0 && prev->end + 1 == e->start) {
        prev->end  = e->end;
        prev->next = e->next;
        pool_free(e);
    }
}

/* ---------- IPC-payload helpers --------------------------------- */
/* Wire format: rsrc_alloc_t / rsrc_request_t live at msg[4..] back-
 * to-back.  Both structs are sized in qsoe_ipcbuf units; we cast
 * msg[4..] into the right struct.  `name` pointers in the wire copy
 * are meaningless (different VSpace); we treat them as 0 / NULL.    */

#define RSRC_PAYLOAD ((unsigned char *)&qsoe_ipcbuf->msg[4])

/* ---------- public API ------------------------------------------ */

void tm_rsrc_init(void)
{
    pool_init();
}

int tm_rsrc_create(pid_t caller, unsigned count)
{
    (void)caller;
    rsrc_alloc_t *p = (rsrc_alloc_t *)RSRC_PAYLOAD;
    for (unsigned i = 0; i < count; ++i) {
        uint16_t cls = (uint16_t)(p[i].flags & RSRCDBMGR_TYPE_MASK);
        if (cls >= RSRCDBMGR_TYPE_COUNT) return -EINVAL;
        tm_rsrc_t *e = pool_alloc();
        if (!e) return -ENOMEM;
        e->class = cls;
        e->start = p[i].start;
        e->end   = p[i].end;
        e->owner = (p[i].flags & RSRCDBMGR_FLAG_USED) ? caller : 0;
        list_insert(e);
        /* Eager neighbour-merge for FREE entries keeps the boot
         * seeding output compact. */
        if (e->owner == 0) merge_neighbours(e);
    }
    return 0;
}

int tm_rsrc_destroy(pid_t caller, unsigned count)
{
    (void)caller;
    rsrc_alloc_t *p = (rsrc_alloc_t *)RSRC_PAYLOAD;
    for (unsigned i = 0; i < count; ++i) {
        uint16_t cls = (uint16_t)(p[i].flags & RSRCDBMGR_TYPE_MASK);
        if (cls >= RSRCDBMGR_TYPE_COUNT) return -EINVAL;
        /* Match exactly on (class, start, end). */
        for (tm_rsrc_t *e = s_class_head[cls]; e; e = e->next) {
            if (e->start == p[i].start && e->end == p[i].end) {
                list_remove(e);
                pool_free(e);
                break;
            }
        }
    }
    return 0;
}

int tm_rsrc_attach(pid_t caller, unsigned count)
{
    rsrc_request_t *p = (rsrc_request_t *)RSRC_PAYLOAD;

    /* Track granted entries so we can roll back on partial failure. */
    tm_rsrc_t *granted[16];
    unsigned   ngranted = 0;
    if (count > 16) return -EINVAL;

    for (unsigned i = 0; i < count; ++i) {
        uint16_t cls = (uint16_t)(p[i].flags & RSRCDBMGR_TYPE_MASK);
        if (cls >= RSRCDBMGR_TYPE_COUNT) goto rollback;

        uint64_t gs, ge;
        tm_rsrc_t *e = find_fit(cls, &p[i], &gs, &ge);
        if (!e) goto rollback;

        if (carve(e, gs, ge, caller, p[i].flags, 0) != 0) goto rollback;

        /* Find the carved entry to record for rollback (it now has
         * owner==caller, class==cls, start==gs).                    */
        for (tm_rsrc_t *c = s_class_head[cls]; c; c = c->next) {
            if (c->owner == caller && c->start == gs && c->end == ge) {
                granted[ngranted++] = c;
                break;
            }
        }
        /* Echo granted range back to caller. */
        p[i].start = gs;
        p[i].end   = ge;
        p[i].length = ge - gs + 1;
    }
    return 0;

rollback:
    for (unsigned j = 0; j < ngranted; ++j) {
        granted[j]->owner = 0;
        merge_neighbours(granted[j]);
    }
    return -ENOSPC;
}

int tm_rsrc_detach(pid_t caller, unsigned count)
{
    rsrc_request_t *p = (rsrc_request_t *)RSRC_PAYLOAD;
    for (unsigned i = 0; i < count; ++i) {
        uint16_t cls = (uint16_t)(p[i].flags & RSRCDBMGR_TYPE_MASK);
        if (cls >= RSRCDBMGR_TYPE_COUNT) return -EINVAL;
        for (tm_rsrc_t *e = s_class_head[cls]; e; e = e->next) {
            if (e->owner == caller &&
                e->start == p[i].start && e->end == p[i].end) {
                e->owner = 0;
                e->flags = 0;
                merge_neighbours(e);
                break;
            }
        }
    }
    return 0;
}

int tm_rsrc_query(pid_t caller, unsigned listcnt, unsigned start,
                  uint32_t type, unsigned *out_written)
{
    (void)caller;
    uint16_t cls = (uint16_t)(type & RSRCDBMGR_TYPE_MASK);
    if (cls >= RSRCDBMGR_TYPE_COUNT) return -EINVAL;

    rsrc_alloc_t *p = (rsrc_alloc_t *)RSRC_PAYLOAD;
    unsigned idx = 0;
    unsigned written = 0;
    for (tm_rsrc_t *e = s_class_head[cls]; e; e = e->next) {
        if (idx++ < start) continue;
        if (listcnt && written >= listcnt) break;
        if (listcnt) {
            p[written].start = e->start;
            p[written].end   = e->end;
            p[written].flags = (uint32_t)e->class |
                               (e->owner ? RSRCDBMGR_FLAG_USED : 0);
            p[written].name  = 0;
        }
        ++written;
    }
    *out_written = written;
    return 0;
}

void tm_rsrc_release_pid(pid_t pid)
{
    if (pid <= 0) return;
    for (unsigned c = 0; c < RSRCDBMGR_TYPE_COUNT; ++c) {
        tm_rsrc_t *e = s_class_head[c];
        while (e) {
            tm_rsrc_t *nxt = e->next;
            if (e->owner == pid) {
                e->owner = 0;
                e->flags = 0;
                merge_neighbours(e);
                /* `e` may have been freed by merge — restart from
                 * the head to avoid use-after-free; rare path. */
                e = s_class_head[c];
                continue;
            }
            e = nxt;
        }
    }
}

/* ---------- boot seeding from syscfg ---------------------------- */

void tm_rsrc_seed_from_syscfg(void)
{
    /* Walk all MEMORY tags and register each (base, size) tuple as a
     * RSRCDBMGR_MEMORY free range.  Multiple memory regions stack
     * cleanly; sorted-insert keeps the class list ordered. */
    unsigned off = 0;
    const void *blob; unsigned blob_len;
    if (tm_syscfg_get(&blob, &blob_len) != 0) return;

    const unsigned char *bp = (const unsigned char *)blob;
    while (off + 4 <= blob_len) {
        uint16_t id  = (uint16_t)(bp[off]   | (bp[off+1] << 8));
        uint16_t len = (uint16_t)(bp[off+2] | (bp[off+3] << 8));
        if (id == 0) break;            /* END sentinel */
        if (id == TM_SYSCFG_TAG_MEMORY && len == 16) {
            uint64_t base = 0, size = 0;
            for (int i = 7; i >= 0; --i) base = (base << 8) | bp[off+4+i];
            for (int i = 7; i >= 0; --i) size = (size << 8) | bp[off+12+i];
            tm_rsrc_t *e = pool_alloc();
            if (!e) return;
            e->class = RSRCDBMGR_MEMORY;
            e->start = base;
            e->end   = base + size - 1;
            e->owner = 0;
            list_insert(e);
        }
        off += 4 + len;
    }
}
