/*
 * mmap.c — libqsoe wire wrapper for TM_REQ_MMAP.
 *
 * Memory always comes from taskman's Memory Manager via TM_REQ_MMAP
 * — no brk anywhere in QSOE.  libqsoe's malloc.c grabs pages here
 * for its mmap-backed pool.
 *
 * v0.8: two flavours behind one entry point.  When `flags & MAP_PHYS`
 * the caller names a physical address (via the POSIX `off` argument,
 * matching QRV's `mmap(NULL, sz, prot, MAP_PHYS|MAP_SHARED, NOFD,
 * phys)` convention).  Otherwise it's an anonymous 2 MiB-granular
 * allocation backed by RAM untyped.
 *
 * Wire layout:
 *   mr0 = length (bytes)
 *   mr1 = flags  (TM_MMAP_FLAG_PHYS, etc.)
 *   mr2 = phys   (only meaningful when flags & PHYS)
 *
 * The public POSIX mmap() entry point lives in userland/libc/qsoe/
 * once the libc side wires up.  Direct callers (drivers, resmgrs)
 * use qsoe_mmap_phys() / qsoe_mmap_anon() helpers below.
 */

#include <qsoe-system.h>
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#define QSOE_MAP_FAILED  ((void *)-1)

/* Match taskman/mem/mem.h.  Kept inline here so a client only needs
 * <qsoe-system.h> to ask for a physical mapping. */
#define QSOE_MAP_PHYS   0x10000

static void *qsoe_mmap_internal(unsigned long length,
                                 unsigned long flags,
                                 unsigned long phys)
{
    if (length == 0) { qsoe_errno = EINVAL; return QSOE_MAP_FAILED; }
    seL4_Word mr0 = (seL4_Word)length;
    seL4_Word mr1 = (seL4_Word)flags;
    seL4_Word mr2 = (seL4_Word)phys;
    seL4_Word mr3 = 0;
    /* Length=3 covers mr0/mr1/mr2 — mr3 unused for MMAP today. */
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_MMAP, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return QSOE_MAP_FAILED; }
    return (void *)(unsigned long)mr0;
}

void *qsoe_mmap(void *addr, unsigned long length, int prot, int flags,
                int fd, long off);
void *qsoe_mmap(void *addr, unsigned long length, int prot, int flags,
                int fd, long off)
{
    (void)addr; (void)prot;
    /* MAP_PHYS: fd ignored, off is the physical address. */
    if (flags & QSOE_MAP_PHYS) {
        unsigned long phys = (unsigned long)off;
        /* Translate POSIX MAP_PHYS into taskman's internal flag.
         * The lower bit (TM_MMAP_FLAG_PHYS = 1) is the wire-side
         * shape; ORing it into `flags` for taskman is fine. */
        return qsoe_mmap_internal(length, 0x1u, phys);
    }
    /* Anonymous path: fd must be -1 / NOFD. */
    if (fd >= 0) { qsoe_errno = ENODEV; return QSOE_MAP_FAILED; }
    (void)off;
    return qsoe_mmap_internal(length, 0, 0);
}
