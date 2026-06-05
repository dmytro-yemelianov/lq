/*
 * munmap.c — munmap() / __munmap() for LQ.
 *
 * Sends TM_REQ_MUNMAP with (vaddr, length) so taskman Page_Unmaps
 * each Mega_Page in the range, CNode_Deletes the frame caps, recycles
 * the slots into taskman_free_slot's pool, and drops the entries from
 * proc->mmap[].  mmap_top stays put (still bump-allocate); the freed
 * VA range becomes a permanent hole, which is fine because mmap
 * always returns a fresh higher VA.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/mman.h>
#include <stdint.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

int __munmap(void *start, size_t length)
{
    if (length == 0) { qsoe_errno = EINVAL; return -1; }

    seL4_Word mr0 = (seL4_Word)(uintptr_t) start;
    seL4_Word mr1 = (seL4_Word) length;
    seL4_Word mr2 = 0;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_MUNMAP, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int) err; return -1; }
    return 0;
}

weak_alias(__munmap, munmap);
