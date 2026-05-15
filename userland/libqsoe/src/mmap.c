/*
 * mmap.c — libqsoe wire wrapper for TM_REQ_MMAP.
 *
 * Recovered from the v0.6.4 syscall_dispatch.c.  Memory always comes
 * from taskman's Memory Manager via TM_REQ_MMAP — no brk anywhere in
 * QSOE.  libqsoe's malloc.c is this function's main caller (it asks
 * for chunks of pages when its mmap-backed pool runs low).
 *
 * The public POSIX mmap() entry point will land in
 * userland/libc/qsoe/mmap.c later, calling this function directly.
 */

#include "../include/qsoe/qrv.h"
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

#define QSOE_MAP_FAILED  ((void *)-1)

void *qsoe_mmap(void *addr, unsigned long length, int prot, int flags,
                int fd, long off);
void *qsoe_mmap(void *addr, unsigned long length, int prot, int flags,
                int fd, long off)
{
    (void)addr; (void)prot; (void)flags; (void)off;
    if (fd >= 0) { qsoe_errno = ENODEV; return QSOE_MAP_FAILED; }
    if (length == 0) { qsoe_errno = EINVAL; return QSOE_MAP_FAILED; }

    seL4_Word mr0 = (seL4_Word)length, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_MMAP, 0, 0, 1);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return QSOE_MAP_FAILED; }
    return (void *)(unsigned long)mr0;
}
