/*
 * libqsoe/src/malloc.c — mmap-only allocator (v0.6.4).
 *
 * Replaces musl's lite_malloc.  The decision: NO brk anywhere in
 * QSOE.  Memory comes from taskman's Memory Manager via TM_REQ_MMAP
 * (see qsoe_mmap in syscall_dispatch.c).  This file implements
 * malloc / free / realloc / calloc + the musl-internal aliases
 * __libc_malloc / __libc_malloc_impl / __libc_free / __libc_realloc
 * that musl uses via `#define malloc __libc_malloc` to defeat
 * application interposers.
 *
 * Strategy
 * --------
 * Single bump-pointer arena, grown on demand:
 *   - On first malloc, mmap() a 2 MiB arena from taskman.
 *   - Carve allocations top-down: each request advances `arena_cur`.
 *   - When the current arena is exhausted, mmap() another 2 MiB
 *     chunk and switch `arena_cur`/`arena_end` to it.  No attempt to
 *     merge contiguous chunks — fine because taskman returns
 *     consecutive Mega_Pages and we'd typically just bump through.
 *   - free() is a no-op.  realloc() always copies into a fresh
 *     allocation.  This is "leak everything", which is correct (just
 *     wasteful) for the v0.6.4 shell.  v0.7 will land a real
 *     freelist-backed allocator.
 *
 * Per-allocation header: an 8-byte length so realloc/calloc know the
 * old size for copies and zero-fills.  Returned pointers are 16-byte
 * aligned per the SysV ABI.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <qsoe-system.h>

#define ARENA_CHUNK     0x200000UL          /* one Mega_Page */
#define ALIGN           16UL
#define ALIGN_UP(x, a)  (((x) + (a) - 1UL) & ~((a) - 1UL))

extern void *qsoe_mmap(void *addr, unsigned long length, int prot, int flags,
                       int fd, long off);

/* Per-allocation header.  Lives immediately below the user pointer;
 * size is the user-visible byte count.  16-byte sized so the user
 * pointer stays 16-aligned. */
struct qsoe_alloc_hdr {
    unsigned long size;
    unsigned long _pad;
};

/* Arena state.  Cheap-and-cheerful: one cursor, grow by mmap when
 * exhausted.  Single-threaded for v0.6.4 — the signal thread doesn't
 * malloc, and qsh is the only user. */
static unsigned char *arena_cur;
static unsigned char *arena_end;

/* Memcpy / memset — we run with -fno-builtin and may be called
 * before any libc setup. */
static void qsoe_malloc_memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
}

static void qsoe_malloc_memset(void *dst, int v, unsigned long n)
{
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)v;
}

static int grow_arena(unsigned long need)
{
    unsigned long ask = ALIGN_UP(need, ARENA_CHUNK);
    void *p = qsoe_mmap(0, ask, 0, 0, -1, 0);
    if (p == (void *)-1) return -1;
    arena_cur = (unsigned char *)p;
    arena_end = arena_cur + ask;
    return 0;
}

static void *qsoe_malloc_internal(unsigned long n)
{
    if (n == 0) n = 1;
    unsigned long total = ALIGN_UP(sizeof(struct qsoe_alloc_hdr) + n, ALIGN);

    if (arena_cur == 0 || (unsigned long)(arena_end - arena_cur) < total) {
        if (grow_arena(total) < 0) { qsoe_errno = ENOMEM; return 0; }
    }

    struct qsoe_alloc_hdr *h = (struct qsoe_alloc_hdr *)arena_cur;
    arena_cur += total;
    h->size = n;
    return (unsigned char *)h + sizeof(*h);
}

void *malloc(unsigned long n);
void *malloc(unsigned long n)
{
    return qsoe_malloc_internal(n);
}

void free(void *p);
void free(void *p) { (void)p; }   /* v0.6.4: leak everything. */

void *realloc(void *p, unsigned long n);
void *realloc(void *p, unsigned long n)
{
    if (!p) return qsoe_malloc_internal(n);
    if (n == 0) { /* glibc-style: free + return NULL */ return 0; }

    struct qsoe_alloc_hdr *h = (struct qsoe_alloc_hdr *)
        ((unsigned char *)p - sizeof(struct qsoe_alloc_hdr));
    unsigned long old = h->size;
    void *q = qsoe_malloc_internal(n);
    if (!q) return 0;
    qsoe_malloc_memcpy(q, p, old < n ? old : n);
    return q;
}

void *calloc(unsigned long nmemb, unsigned long size);
void *calloc(unsigned long nmemb, unsigned long size)
{
    /* Overflow check.  Returning NULL is the POSIX-correct response. */
    if (size != 0 && nmemb > (~0UL) / size) { qsoe_errno = ENOMEM; return 0; }
    unsigned long n = nmemb * size;
    void *p = qsoe_malloc_internal(n);
    if (!p) return 0;
    qsoe_malloc_memset(p, 0, n);
    return p;
}

/* musl-internal aliases.  Several musl translation units use
 * `#define malloc __libc_malloc` (etc.) so application interposers
 * can't intercept libc-internal allocations.  Forward them here. */
void *__libc_malloc(unsigned long n);
void *__libc_malloc(unsigned long n) { return qsoe_malloc_internal(n); }

void *__libc_malloc_impl(unsigned long n);
void *__libc_malloc_impl(unsigned long n) { return qsoe_malloc_internal(n); }

void __libc_free(void *p);
void __libc_free(void *p) { (void)p; }

void *__libc_realloc(void *p, unsigned long n);
void *__libc_realloc(void *p, unsigned long n) { return realloc(p, n); }

void *__libc_calloc(unsigned long nmemb, unsigned long size);
void *__libc_calloc(unsigned long nmemb, unsigned long size)
{
    return calloc(nmemb, size);
}
