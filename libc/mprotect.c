/*
 * mprotect.c -- LQ seam for POSIX mprotect().
 *
 * Routes the protection change to taskman (TM_REQ_MPROTECT), which holds
 * the page's frame cap and re-maps it with the requested rights.  The
 * caller rtld is the dynamic linker's RELRO pass: after applying
 * relocations it calls mprotect(relro_page, relro_size, PROT_READ) to make
 * the GOT / .data.rel.ro / .dynamic read-only.  Taskman keeps an invokeable
 * frame cap for every page in each loaded object's PT_GNU_RELRO range (see
 * lq/taskman/proc/spawn.c) precisely so this call can take effect.
 *
 * addr must be page-aligned and len > 0; taskman rounds len up to a page.
 * Returns 0 on success, -1 with errno set on failure (EACCES if the
 * requested protection can't be applied to that range, ENOMEM if the range
 * isn't mapped, EINVAL on a misaligned addr / zero length).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/mman.h>
#include <errno.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>     /* QSOE_CAP_TASKMAN_EP */
#include <qsoe/tm_msgs.h>

#include "sel4_types.h"
#include "qsoe_invoke.h"

int mprotect(void *addr, size_t len, int prot)
{
    seL4_Word mr0 = (seL4_Word)(unsigned long)addr;
    seL4_Word mr1 = (seL4_Word)len;
    seL4_Word mr2 = (seL4_Word)(unsigned)prot;
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_MPROTECT, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                             &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) { qsoe_errno = (int)err; return -1; }
    return 0;
}
