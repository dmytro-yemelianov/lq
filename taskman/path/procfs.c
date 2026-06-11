/*
 * path/procfs.c -- LQ wiring for the synthetic read-only /proc tree.
 *
 * Glue between the OS-independent /proc model (libtaskman <tm_procfs.h>)
 * and LQ's in-taskman resmgr plumbing.  tm_procfs_populate() hands the
 * core LQ's process-table accessors; the per-fd handlers below are
 * dispatched by path/io.c on (taskman, TM_PROCFS_CHID) and keep per-open
 * state in the connection ctx, mirroring sysfs.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include "procfs.h"
#include "../proc/proc.h"         /* tm_process_*, tm_connection_*_ctx */
#include <tm_procfs.h>
#include <qsoe/ipcbuf.h>
#include <errno.h>

/* ---- LQ process-table accessors handed to the shared core --------- */

static int lq_procfs_get(int pid, struct tm_procfs_proc *out)
{
    tm_process_t *p = tm_process_lookup((pid_t)pid);
    if (p == 0 || !p->in_use) return 0;
    out->pid   = (int)p->pid;
    out->ppid  = (int)p->parent_pid;
    out->state = (p->exit_state >= 2) ? 1 : 0;   /* 2 = terminated/zombie */
    unsigned i = 0;
    while (p->name[i] != '\0' && i < TM_PROCFS_NAME_MAX - 1) {
        out->name[i] = p->name[i];
        ++i;
    }
    out->name[i] = '\0';
    return 1;
}

/* Lowest live pid >= `from`.  LQ's table is a linear array (not pid-
 * indexed), so this is a min-search rather than a forward scan. */
static int lq_procfs_next(int from, struct tm_procfs_proc *out)
{
    if (from < 0) from = 0;
    int best = -1;
    for (int i = 0; i < TM_MAX_PROCESSES; ++i) {
        tm_process_t *p = tm_process_by_index(i);
        if (p == 0 || !p->in_use) continue;
        int pid = (int)p->pid;
        if (pid < from) continue;
        if (best < 0 || pid < best) best = pid;
    }
    if (best < 0) return 0;
    lq_procfs_get(best, out);
    return best;
}

void tm_procfs_populate(void)
{
    tm_procfs_init(lq_procfs_get, lq_procfs_next);
}

/* ---- Per-open state in the connection ctx -------------------------
 *
 * ctx[0] packs (node-kind << 32) | pid:
 *   PK_ROOT   (1) -- the /proc directory          (pid unused)
 *   PK_PIDDIR (2) -- a /proc/<pid> directory
 *   PK_INFO   (3) -- a /proc/<pid>/info file
 * ctx[1] is the read offset (info) or readdir cursor (dirs).  Kept in
 * the ctx (not a local pool) so it survives the dup2 the shell does on
 * `read X < /proc/<pid>/info`, exactly as sysfs documents. */

#define PK_ROOT    1u
#define PK_PIDDIR  2u
#define PK_INFO    3u

static unsigned long pk_pack(unsigned kind, int pid)
{
    return ((unsigned long)kind << 32) | (unsigned)pid;
}
static unsigned pk_kind(unsigned long c0) { return (unsigned)(c0 >> 32); }
static int      pk_pid(unsigned long c0)  { return (int)(unsigned)(c0 & 0xffffffffu); }

int tm_procfs_open(const char *path, seL4_Word badge)
{
    int pid = 0;
    int k = tm_procfs_resolve(path, &pid);
    if (k == 0) return -ENOENT;
    unsigned kind = (k == 1) ? PK_ROOT : (k == 2) ? PK_PIDDIR : PK_INFO;
    return tm_connection_set_ctx(badge, pk_pack(kind, (k == 1) ? 0 : pid), 0);
}

int tm_procfs_read(seL4_Word badge, unsigned want, unsigned *out_got)
{
    *out_got = 0;
    unsigned long c0 = 0, c1 = 0;
    if (tm_connection_get_ctx(badge, &c0, &c1) != 0) return -EBADF;
    if (pk_kind(c0) != PK_INFO) return -EISDIR;

    char buf[TM_PROCFS_INFO_MAX];
    unsigned len = tm_procfs_info(pk_pid(c0), buf, sizeof buf);
    unsigned long pos = c1;
    unsigned remain = (pos < len) ? (unsigned)(len - pos) : 0;
    unsigned n = (want < remain) ? want : remain;

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < n; ++i) dst[i] = (unsigned char)buf[pos + i];
    *out_got = n;
    return tm_connection_set_ctx(badge, c0, pos + n);
}

int tm_procfs_readdir(seL4_Word badge, char *name_out,
                      unsigned *namelen_out, int *d_type_out)
{
    *namelen_out = 0;
    unsigned long c0 = 0, c1 = 0;
    if (tm_connection_get_ctx(badge, &c0, &c1) != 0) return -EBADF;
    unsigned kind = pk_kind(c0);
    if (kind == PK_INFO) return -ENOTDIR;

    if (kind == PK_ROOT) {
        unsigned long cursor = c1;
        if (!tm_procfs_readdir_root(&cursor, name_out, namelen_out, d_type_out))
            return -ENOENT;                       /* end of dir */
        return tm_connection_set_ctx(badge, c0, cursor);
    }
    /* PK_PIDDIR: single "info" entry. */
    if (!tm_procfs_readdir_piddir(c1, name_out, namelen_out, d_type_out))
        return -ENOENT;
    return tm_connection_set_ctx(badge, c0, c1 + 1);
}

int tm_procfs_fstat(seL4_Word badge, tm_stat_t *out)
{
    unsigned long c0 = 0, c1 = 0;
    if (tm_connection_get_ctx(badge, &c0, &c1) != 0) return -EBADF;

    unsigned char *p = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof *out; ++i) p[i] = 0;
    out->st_blksize = 4096;
    out->st_dev     = 2;

    if (pk_kind(c0) == PK_INFO) {
        char buf[TM_PROCFS_INFO_MAX];
        unsigned len = tm_procfs_info(pk_pid(c0), buf, sizeof buf);
        out->st_ino   = 1000 + pk_pid(c0);
        out->st_mode  = TM_S_IFREG | 0444;
        out->st_nlink = 1;
        out->st_size  = (long)len;
    } else {
        out->st_ino   = (pk_kind(c0) == PK_ROOT) ? 99 : (1000 + pk_pid(c0));
        out->st_mode  = TM_S_IFDIR | 0555;
        out->st_nlink = 2;
    }
    return 0;
}

int tm_procfs_lseek(seL4_Word badge, int whence, long offset, long *out_off)
{
    unsigned long c0 = 0, c1 = 0;
    if (tm_connection_get_ctx(badge, &c0, &c1) != 0) return -EBADF;
    if (pk_kind(c0) != PK_INFO) return -EISDIR;   /* dirs advance via readdir */

    char buf[TM_PROCFS_INFO_MAX];
    unsigned len = tm_procfs_info(pk_pid(c0), buf, sizeof buf);

    long newpos;
    switch (whence) {
    case 0: newpos = offset;              break;  /* SEEK_SET */
    case 1: newpos = (long)c1 + offset;   break;  /* SEEK_CUR */
    case 2: newpos = (long)len + offset;  break;  /* SEEK_END */
    default: return -EINVAL;
    }
    if (newpos < 0) return -EINVAL;
    *out_off = newpos;
    return tm_connection_set_ctx(badge, c0, (unsigned long)newpos);
}

int tm_procfs_close(seL4_Word badge)
{
    /* No external state: the connection ctx is reclaimed on detach. */
    (void)badge;
    return 0;
}
