/*
 * lq/libc/qsoe/posix_spawn.c -- POSIX posix_spawn(3) for QSOE/L.
 *
 * Wire-level shape: we hand taskman the (path, argv, envp) by writing
 * a packed (path\0 argv[0]\0 ... envp[envc-1]\0) blob into a one-page
 * mmap'd side channel, then send TM_REQ_SPAWN naming the page address,
 * the blob length, argc and envc.  Taskman cap-copies + scratch-maps
 * the page, parses, and bottoms out into tm_process_create_by_name.
 *
 * Why a side channel and not the IPC message buffer?  seL4's MR
 * payload tops out around ~120 bytes after the wire-fixed headers --
 * enough for a single short path but not for a real argv/envp.  One
 * 4 KiB page is the smallest unit the LQ memory manager hands out
 * (TM_REQ_MMAP rounds up to 2 MiB Mega_Pages, so we waste 2 MiB - 4
 * KiB once per spawn -- acceptable for now; pooling lands later).
 *
 * file_actions and attrp are not yet honoured (we inherit stdio
 * unconditionally; the future fd-actions replay happens taskman-side
 * once TM_REQ_SPAWN gains a file-actions list parameter).
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include <spawn.h>
#include <sys/mman.h>
#include <errno.h>

#include "sel4_types.h"
#include "qsoe_invoke.h"

#define SPAWN_PAGE_BYTES   4096
#define SPAWN_MAX_ARGC     16
#define SPAWN_MAX_ENVC     16

static unsigned qstrlen(const char *s)
{
    unsigned n = 0;
    while (s && s[n]) ++n;
    return n;
}

static int pack_str(char *buf, unsigned cap, unsigned *off,
                     const char *s, unsigned len)
{
    if (*off + len + 1 > cap) return -1;
    for (unsigned i = 0; i < len; ++i) buf[*off + i] = s[i];
    buf[*off + len] = 0;
    *off += len + 1;
    return 0;
}

int posix_spawn(pid_t * __restrict pid,
                const char * __restrict path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t * __restrict attrp,
                char *const * __restrict argv,
                char *const * __restrict envp)
{
    (void)fa; (void)attrp;
    if (!path || !path[0]) return EINVAL;

    /* Count argv / envp; the v0.8 wire caps both at 16 entries. */
    int argc = 0, envc = 0;
    if (argv) while (argv[argc]) {
        if (++argc > SPAWN_MAX_ARGC) return E2BIG;
    }
    if (envp) while (envp[envc]) {
        if (++envc > SPAWN_MAX_ENVC) return E2BIG;
    }

    /* Get a fresh page from taskman to stage the packed blob.  The
     * memory manager hands out Mega_Pages -- we use the first 4 KiB
     * and leave the rest of the slot alone (the cost of a clean,
     * stateless side channel; pooling waits for Stage-B). */
    void *page = __mmap(0, SPAWN_PAGE_BYTES,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) return ENOMEM;

    char *buf = page;
    unsigned off = 0;
    unsigned plen = qstrlen(path);
    if (pack_str(buf, SPAWN_PAGE_BYTES, &off, path, plen) != 0) {
        return E2BIG;
    }
    for (int i = 0; i < argc; ++i) {
        const char *s = argv[i];
        if (pack_str(buf, SPAWN_PAGE_BYTES, &off, s, qstrlen(s)) != 0)
            return E2BIG;
    }
    for (int i = 0; i < envc; ++i) {
        const char *s = envp[i];
        if (pack_str(buf, SPAWN_PAGE_BYTES, &off, s, qstrlen(s)) != 0)
            return E2BIG;
    }

    seL4_Word mr0 = (seL4_Word)(uintptr_t)page;
    seL4_Word mr1 = (seL4_Word)off;
    seL4_Word mr2 = (seL4_Word)argc;
    seL4_Word mr3 = (seL4_Word)envc;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_SPAWN, 0, 0, 4);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    /* Release the args page either way -- taskman has finished
     * reading it (success) or never used it (failure).  Without this
     * the per-proc mmap tracker fills after ~64 spawns. */
    (void) __munmap(page, SPAWN_PAGE_BYTES);
    if (err != 0) {
        /* posix_spawn returns the errno value rather than setting
         * qsoe_errno + returning -1 (POSIX wart).  Pass the wire
         * error code straight through. */
        return (int)err;
    }
    if (pid) *pid = (pid_t)mr0;
    return 0;
}

/* posix_spawnp() lives in libc/1d/posix_spawnp.c (musl shape): it
 * builds an attr whose __fn is __execvpe and tail-calls into us. */
