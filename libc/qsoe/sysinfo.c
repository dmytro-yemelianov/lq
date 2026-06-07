/*
 * qsoe/sysinfo.c -- qsoe_sysinfo seam for LQ: not wired yet.
 *
 * On QSOE/N the groups are one kernel trap (SYS_SYSINFO); on LQ the
 * same record groups will be assembled from taskman (which owns the
 * thread/timer/IRQ knowledge on seL4) once a TM_REQ opcode lands.
 * Until then: announce once, return ENOSYS -- ps/sysinfo degrade
 * gracefully instead of failing to load over a missing symbol.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <errno.h>
#include <qsoe/sysinfo.h>

int qsoe_sysinfo(unsigned group, void *buf, unsigned max_records)
{
    (void) buf; (void) max_records;
    static int announced;
    if (!announced) {
        announced = 1;
        fprintf(stderr, "STUB: qsoe_sysinfo(group=%u) returning ENOSYS "
                        "(LQ taskman opcode not wired yet)\n", group);
    }
    errno = ENOSYS;
    return -1;
}
