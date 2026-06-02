/*
 * mremap.c — mremap() / __mremap() for LQ.
 *
 * LQ taskman has no TM_REQ_MREMAP opcode in v0.x.  The shared OS-
 * independent libc body's mallocng links to __mremap by hard reference
 * (the realloc-grow path), so the symbol must exist.  Per the project
 * rule "every stub announces itself", first call writes a STUB line to
 * fd 2 then returns MAP_FAILED with ENOSYS — honest behaviour the
 * caller can detect.  A real implementation needs a corresponding LQ
 * taskman opcode + VSpace cap move — Stage-B work.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <r_tty@yahoo.co.uk>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <sys/mman.h>
#include <stdint.h>
#include <unistd.h>
#include <qsoe-system.h>

static void announce_once(void)
{
    static int announced;
    if (announced) return;
    announced = 1;
    static const char msg[] =
        "STUB: mremap -> MAP_FAILED/ENOSYS (LQ taskman has no "
        "TM_REQ_MREMAP yet)\n";
    (void) write(2, msg, sizeof msg - 1);
}

void *__mremap(void *old_addr, size_t old_length, size_t new_length, int flags, ...)
{
    (void) old_addr; (void) old_length; (void) new_length; (void) flags;
    announce_once();
    qsoe_errno = ENOSYS;
    return MAP_FAILED;
}

weak_alias(__mremap, mremap);
