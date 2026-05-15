/*
 * userland/sbin/repath — re-point a pathmgr entry.
 *
 * One-shot CLI front-end for qsoe_pathmgr_resolve() + qsoe_pathmgr_repath().
 * Two usages:
 *
 *   repath <target_path> <source_path>
 *       Look up source_path in the pathmgr; rebind target_path to the
 *       same (pid, chid, kind).  This is the QNX-style usage and the
 *       one /sbin/init relies on — it doesn't have to know any pids.
 *
 *   repath <target_path> <pid> <chid>
 *       Explicit form (handler_kind defaults to 0 / external).  Useful
 *       for tests and unusual binding rewires.
 *
 * Per CLAUDE.md "Requirements for resource managers" this binary uses
 * only libqsoe + libc — no direct seL4 syscalls.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <qsoe-system.h>

int main(int argc, char **argv, char **envp)
{
    (void)envp;
    if (argc == 3) {
        const char *target = argv[1];
        const char *source = argv[2];
        pid_t    spid  = 0;
        int      schid = 0;
        unsigned skind = 0;
        if (qsoe_pathmgr_resolve(source, &spid, &schid, &skind) != 0) {
            fprintf(stderr, "repath: %s: errno=%d\n", source, qsoe_errno);
            return 1;
        }
        if (qsoe_pathmgr_repath(target, spid, schid, skind) != 0) {
            fprintf(stderr, "repath: %s: errno=%d\n", target, qsoe_errno);
            return 1;
        }
        return 0;
    }
    if (argc == 4) {
        const char *path = argv[1];
        pid_t pid  = (pid_t)atoi(argv[2]);
        int   chid = atoi(argv[3]);
        if (qsoe_pathmgr_repath(path, pid, chid, /*handler_kind=*/0) != 0) {
            fprintf(stderr, "repath: %s: errno=%d\n", path, qsoe_errno);
            return 1;
        }
        return 0;
    }
    fprintf(stderr,
            "usage: repath <target> <source>\n"
            "       repath <path> <pid> <chid>\n");
    return 1;
}
