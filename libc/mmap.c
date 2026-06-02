/*
 * mmap.c — mmap() and __mmap() for LQ — install a memory mapping via
 *          taskman over seL4 IPC.
 *
 * The shared OS-independent libc body's mallocng + __tz / etc. call
 * __mmap; we expose both the public POSIX entry point and the musl-
 * internal __mmap.  Both build a TM_REQ_MMAP wire request, send it
 * to taskman via qsoe_invoke(), and return the start address.
 *
 * Wire shape matches userland/libqsoe/src/mmap.c (Phase 2 reaches
 * through libqsoe; Phase 2.5 will fold libqsoe in and this file
 * absorbs the qsoe_invoke wrapper directly).  Only the anonymous
 * path (the shared body's only use) lives here; QSOE_MAP_PHYS stays
 * in libqsoe's qsoe_mmap() helper for now.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <r_tty@yahoo.co.uk>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/mman.h>
#include <stdint.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

void *__mmap(void *start, size_t length, int prot, int flags, int fd, off_t off)
{
    (void) start; (void) prot; (void) flags; (void) fd; (void) off;
    if (length == 0) { qsoe_errno = EINVAL; return MAP_FAILED; }

    seL4_Word mr0 = (seL4_Word) length;
    seL4_Word mr1 = 0;                          /* anonymous (not MAP_PHYS) */
    seL4_Word mr2 = 0;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_MMAP, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int) err; return MAP_FAILED; }
    return (void *) (uintptr_t) mr0;
}

weak_alias(__mmap, mmap);
