/*
 * path/io.c — IO dispatch handlers (OPEN / CLOSE / IO_WRITE / IO_READ).
 *
 * Carved out of v0.6.4's main.c switch.  main.c still owns the
 * request-loop and unpacks MRs; it calls these functions with the
 * raw arguments already extracted, and forwards their reply values
 * back through ipc-buffer / MR returns.
 */

#include "path.h"
#include "pathmgr.h"
#include "cpiofs.h"
#include "pmdir.h"
#include "sysfs.h"
#include "procfs.h"
#include "../sys/console.h"
#include "../sys/devnull.h"
#include "../sys/devzero.h"
#include "../proc/proc.h"
#include "../qsoe_invoke.h"
#include <qsoe/tm_msgs.h>

/* TM_REQ_OPEN body.  Walks the path manager, picks the right resmgr,
 * mints a badged Send-cap into the caller's CSpace, attaches per-fd
 * state where the handler needs it (cpiofs stashes data+size). */
int tm_io_open(pid_t caller, unsigned path_len, seL4_CPtr *out_slot)
{
    if (path_len == 0 || path_len >= 128) return -EINVAL;

    static char s_open_path[128];
    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < path_len; ++i) s_open_path[i] = (char)src[i];
    s_open_path[path_len] = 0;

    tm_pathmgr_obj_t obj;
    unsigned consumed = 0;
    int rc = tm_pathmgr_resolve(s_open_path, &obj, &consumed);
    if (rc) return rc;

    /* ConnectAttach mints a badged Send cap on (server_pid, server_chid). */
    seL4_CPtr slot = 0;
    rc = tm_connect_attach(caller, obj.server_pid, obj.server_chid, 0, &slot);
    if (rc) return rc;

    /* Per-fd state for cpiofs: stash (data, size) of the resolved
     * file so subsequent IO_READ can resume from the right offset. */
    if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CPIOFS) {
        seL4_Word badge = 0;
        if (tm_connection_badge_by_slot(caller, slot, &badge) == 0) {
            int orc = tm_cpiofs_open(s_open_path, consumed, badge);
            if (orc) {
                tm_connect_detach(caller, slot);
                return orc;
            }
        }
    } else if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_PMDIR) {
        /* Synthetic dir backed by pathmgr's own tree (e.g. /dev). */
        seL4_Word badge = 0;
        if (tm_connection_badge_by_slot(caller, slot, &badge) == 0) {
            int orc = tm_pmdir_open(s_open_path, badge);
            if (orc) {
                tm_connect_detach(caller, slot);
                return orc;
            }
        }
    } else if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_SYSFS) {
        /* Synthetic read-only /sys (file or the /sys directory). */
        seL4_Word badge = 0;
        if (tm_connection_badge_by_slot(caller, slot, &badge) == 0) {
            int orc = tm_sysfs_open(s_open_path, badge);
            if (orc) {
                tm_connect_detach(caller, slot);
                return orc;
            }
        }
    } else if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_PROCFS) {
        /* Synthetic read-only /proc (root, a pid dir, or an info file). */
        seL4_Word badge = 0;
        if (tm_connection_badge_by_slot(caller, slot, &badge) == 0) {
            int orc = tm_procfs_open(s_open_path, badge);
            if (orc) {
                tm_connect_detach(caller, slot);
                return orc;
            }
        }
    }
    *out_slot = slot;
    return 0;
}

int tm_io_close(pid_t caller, seL4_Word badge)
{
    (void)caller;
    /* Step 1 of two-step close: notify the owning resmgr so it can
     * decrement counts / free per-fd state.  The cap itself is
     * reclaimed by libc's follow-up TM_REQ_DETACH_CAP. */
    pid_t srv_pid = 0;
    int   srv_chid = 0;
    if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) return -EBADF;

    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
        return tm_cpiofs_close(badge);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CONSOLE_CHID) {
        return 0;   /* console has no per-fd state */
    }
    if (srv_pid == QSOE_PID_TASKMAN &&
        (srv_chid == TM_DEVNULL_CHID || srv_chid == TM_DEVZERO_CHID)) {
        return 0;   /* /dev/null and /dev/zero are stateless too */
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_PMDIR_CHID) {
        return tm_pmdir_close(badge);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_SYSFS_CHID) {
        return tm_sysfs_close(badge);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_PROCFS_CHID) {
        return tm_procfs_close(badge);
    }
    /* External resmgrs (e.g. /sbin/pipe): they receive TM_REQ_CLOSE
     * directly on their own channel (the fd's cap points at them);
     * taskman never sees those.  Reply cleanly. */
    return 0;
}

/* IO_WRITE: route by badge → channel → resmgr handler. */
int tm_io_write(pid_t caller, seL4_Word badge, unsigned bytes,
                unsigned *out_written)
{
    (void)caller;
    pid_t srv_pid = 0;
    int srv_chid = 0;
    if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) return -EBADF;

    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CONSOLE_CHID) {
        *out_written = tm_console_write(bytes);
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_DEVNULL_CHID) {
        *out_written = tm_devnull_write(bytes);
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_DEVZERO_CHID) {
        *out_written = tm_devzero_write(bytes);
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
        return -EROFS;  /* cpiofs is read-only */
    }
    return -ENOSYS;  /* external resmgr — v0.6+ extends here */
}

int tm_io_read(pid_t caller, seL4_Word badge, unsigned want,
               unsigned *out_got)
{
    (void)caller;
    pid_t srv_pid = 0;
    int srv_chid = 0;
    if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) return -EBADF;

    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CONSOLE_CHID) {
        return tm_console_read(want, out_got);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_DEVNULL_CHID) {
        return tm_devnull_read(want, out_got);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_DEVZERO_CHID) {
        return tm_devzero_read(want, out_got);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
        return tm_cpiofs_read(badge, want, out_got);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_SYSFS_CHID) {
        return tm_sysfs_read(badge, want, out_got);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_PROCFS_CHID) {
        return tm_procfs_read(badge, want, out_got);
    }
    return -ENOSYS;
}

/* UNLINK: resolve via pathmgr; route to the resmgr that owns the
 * path.  In v0.7 every QSOE resmgr is read-only, so successful
 * resolution lands on a deterministic EROFS reply.  ENOENT comes
 * out of the cpiofs probe; EPERM keeps callers from unlinking
 * device nodes that live under /dev. */
int tm_unlink(pid_t caller, unsigned path_len)
{
    (void)caller;
    if (path_len == 0 || path_len >= 128) return -EINVAL;

    static char s_path[128];
    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < path_len; ++i) s_path[i] = (char)src[i];
    s_path[path_len] = 0;

    tm_pathmgr_obj_t obj;
    unsigned consumed = 0;
    int rc = tm_pathmgr_resolve(s_path, &obj, &consumed);
    if (rc) return rc;

    if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CPIOFS) {
        /* Strip the prefix the resolve consumed; skip any extra
         * slashes; ask cpiofs whether the file exists.  If yes →
         * EROFS (read-only fs).  If no → ENOENT. */
        const char *name = s_path + consumed;
        while (*name == '/') ++name;
        if (*name == 0) return -EISDIR;
        if (tm_cpiofs_probe(name) != 0) return -ENOENT;
        return -EROFS;
    }
    if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CONSOLE ||
        obj.handler_kind == PATHMGR_HANDLER_TASKMAN_NULL ||
        obj.handler_kind == PATHMGR_HANDLER_TASKMAN_ZERO) {
        /* Device nodes — unlinking them has no meaning under QSOE.
         * POSIX-y reply: EPERM. */
        return -EPERM;
    }
    /* External resmgrs forward unlink through their own protocol;
     * not wired yet. */
    return -ENOSYS;
}

/* FSTAT: route by badge → channel → resmgr, fill *out via the
 * resmgr's stat helper, return its size for the IPC framing. */
int tm_fstat(pid_t caller, seL4_Word badge, unsigned *out_bytes)
{
    (void)caller;
    pid_t srv_pid = 0;
    int   srv_chid = 0;
    if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) return -EBADF;

    tm_stat_t *out = (tm_stat_t *)&qsoe_ipcbuf->msg[4];

    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CONSOLE_CHID) {
        int rc = tm_console_stat(out);
        if (rc) return rc;
        *out_bytes = (unsigned)sizeof *out;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_DEVNULL_CHID) {
        int rc = tm_devnull_stat(out);
        if (rc) return rc;
        *out_bytes = (unsigned)sizeof *out;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_DEVZERO_CHID) {
        int rc = tm_devzero_stat(out);
        if (rc) return rc;
        *out_bytes = (unsigned)sizeof *out;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
        int rc = tm_cpiofs_stat(badge, out);
        if (rc) return rc;
        *out_bytes = (unsigned)sizeof *out;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_PMDIR_CHID) {
        int rc = tm_pmdir_stat(out);
        if (rc) return rc;
        *out_bytes = (unsigned)sizeof *out;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_SYSFS_CHID) {
        int rc = tm_sysfs_fstat(badge, out);
        if (rc) return rc;
        *out_bytes = (unsigned)sizeof *out;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_PROCFS_CHID) {
        int rc = tm_procfs_fstat(badge, out);
        if (rc) return rc;
        *out_bytes = (unsigned)sizeof *out;
        return 0;
    }
    /* External resmgrs: route a stat probe over the connection
     * once their wire protocol exists.  Not wired yet. */
    return -ENOSYS;
}

/* READLINK: resolve via pathmgr.  QSOE v0.7 has no symbolic links,
 * so any path that resolves successfully isn't a symlink and the
 * POSIX-correct answer is EINVAL.  Misses fall out as ENOENT.
 * cpiofs is the only resmgr that owns actual paths today; its
 * probe distinguishes ENOENT from "exists but not a symlink". */
int tm_readlink(pid_t caller, unsigned path_len, unsigned *out_bytes)
{
    (void)caller;
    *out_bytes = 0;
    if (path_len == 0 || path_len >= 128) return -EINVAL;

    static char s_path[128];
    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < path_len; ++i) s_path[i] = (char)src[i];
    s_path[path_len] = 0;

    tm_pathmgr_obj_t obj;
    unsigned consumed = 0;
    int rc = tm_pathmgr_resolve(s_path, &obj, &consumed);
    if (rc) return rc;

    if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CPIOFS) {
        const char *name = s_path + consumed;
        while (*name == '/') ++name;
        if (*name == 0) return -EINVAL;   /* "/" is a dir, not a link */
        if (tm_cpiofs_probe(name) != 0) return -ENOENT;
        return -EINVAL;                    /* exists but not a symlink */
    }
    if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CONSOLE ||
        obj.handler_kind == PATHMGR_HANDLER_TASKMAN_NULL ||
        obj.handler_kind == PATHMGR_HANDLER_TASKMAN_ZERO) {
        return -EINVAL;                    /* device nodes aren't symlinks */
    }
    return -EINVAL;
}

/* LSEEK: route by badge → channel → resmgr.  cpiofs holds the
 * per-fd offset in its connection ctx; console / pipes return
 * ESPIPE per POSIX. */
int tm_lseek(pid_t caller, seL4_Word badge, int whence, long offset,
             long *out_off)
{
    (void)caller;
    pid_t srv_pid = 0;
    int   srv_chid = 0;
    if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) return -EBADF;

    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CONSOLE_CHID) {
        return -ESPIPE;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
        return tm_cpiofs_lseek(badge, whence, offset, out_off);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_SYSFS_CHID) {
        return tm_sysfs_lseek(badge, whence, offset, out_off);
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_PROCFS_CHID) {
        return tm_procfs_lseek(badge, whence, offset, out_off);
    }
    return -ESPIPE;
}

/* READDIR: route by badge to the resmgr's readdir handler.  Only
 * cpiofs implements directories today; the console doesn't (it's
 * a character device).  Reply payload encodes one entry: a single
 * d_type byte followed by the NUL-terminated name. */
int tm_readdir(pid_t caller, seL4_Word badge, unsigned *out_bytes)
{
    (void)caller;
    pid_t srv_pid = 0;
    int   srv_chid = 0;
    if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) return -EBADF;

    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CONSOLE_CHID) {
        return -ENOTDIR;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
        unsigned char *p = (unsigned char *)&qsoe_ipcbuf->msg[4];
        char name[256];
        unsigned namelen = 0;
        int d_type = 0;
        int rc = tm_cpiofs_readdir(badge, name, &namelen, &d_type);
        if (rc) return rc;
        p[0] = (unsigned char)d_type;
        for (unsigned i = 0; i < namelen; ++i) p[1 + i] = (unsigned char)name[i];
        p[1 + namelen] = 0;
        *out_bytes = 1 + namelen + 1;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_PMDIR_CHID) {
        unsigned char *p = (unsigned char *)&qsoe_ipcbuf->msg[4];
        char name[256];
        unsigned namelen = 0;
        int d_type = 0;
        int rc = tm_pmdir_readdir(badge, name, &namelen, &d_type);
        if (rc) return rc;
        p[0] = (unsigned char)d_type;
        for (unsigned i = 0; i < namelen; ++i) p[1 + i] = (unsigned char)name[i];
        p[1 + namelen] = 0;
        *out_bytes = 1 + namelen + 1;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_SYSFS_CHID) {
        unsigned char *p = (unsigned char *)&qsoe_ipcbuf->msg[4];
        char name[256];
        unsigned namelen = 0;
        int d_type = 0;
        int rc = tm_sysfs_readdir(badge, name, &namelen, &d_type);
        if (rc) return rc;
        p[0] = (unsigned char)d_type;
        for (unsigned i = 0; i < namelen; ++i) p[1 + i] = (unsigned char)name[i];
        p[1 + namelen] = 0;
        *out_bytes = 1 + namelen + 1;
        return 0;
    }
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_PROCFS_CHID) {
        unsigned char *p = (unsigned char *)&qsoe_ipcbuf->msg[4];
        char name[256];
        unsigned namelen = 0;
        int d_type = 0;
        int rc = tm_procfs_readdir(badge, name, &namelen, &d_type);
        if (rc) return rc;
        p[0] = (unsigned char)d_type;
        for (unsigned i = 0; i < namelen; ++i) p[1 + i] = (unsigned char)name[i];
        p[1 + namelen] = 0;
        *out_bytes = 1 + namelen + 1;
        return 0;
    }
    return -ENOSYS;
}

/* ACCESS: probe path existence.  v0.7 reduces to "does this path
 * resolve, and (for cpiofs) does the file exist".  Mode bits are
 * ignored — every QSOE process is root and every cpiofs file is
 * readable; permissions arrive when we have multi-user state. */
int tm_access(pid_t caller, unsigned path_len)
{
    (void)caller;
    if (path_len == 0 || path_len >= 128) return -EINVAL;

    static char s_path[128];
    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < path_len; ++i) s_path[i] = (char)src[i];
    s_path[path_len] = 0;

    tm_pathmgr_obj_t obj;
    unsigned consumed = 0;
    int rc = tm_pathmgr_resolve(s_path, &obj, &consumed);
    if (rc) return rc;

    if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CPIOFS) {
        const char *name = s_path + consumed;
        while (*name == '/') ++name;
        if (*name == 0) return 0;   /* "/" exists */
        return tm_cpiofs_probe(name);
    }
    /* console & external resmgrs: pathmgr_resolve succeeding means
     * the resmgr owns the path and "exists" for the access() purpose. */
    return 0;
}

/* PIPE_CREATE: locate the pipe manager via pathmgr at /dev/pipe, then
 * mint two badged Send caps on its channel into the caller's CSpace.
 * Badges encode (unique_id << 1) | direction-bit so the pipe manager
 * can route IO requests and tell read-end from write-end. */
int tm_pipe_create(pid_t caller, seL4_CPtr *out_read_slot,
                   seL4_CPtr *out_write_slot)
{
    /* Resolve /dev/pipe; this works once the pipe manager has
     * tm_pathmgr_register'd itself. */
    tm_pathmgr_obj_t obj;
    unsigned consumed = 0;
    if (tm_pathmgr_resolve("/dev/pipe", &obj, &consumed) != 0) {
        return -ENOENT;
    }
    int chidx = tm_channel_index(obj.server_pid, obj.server_chid);
    if (chidx < 0) return -ESRCH;
    tm_channel_t *gch = tm_channels_array();
    seL4_CPtr master = gch[chidx].master;
    if (!master) return -ESRCH;

    tm_process_t *client = tm_process_lookup(caller);
    if (!client) return -ESRCH;

    /* Allocate a fresh unique id.  Monotonic; we don't recycle —
     * the pipe manager's pool is finite, so a very long-running
     * shell can eventually hit EMFILE on pipe creation.  Recycling
     * is a v0.8 enhancement (it needs an "id-freed" notification
     * from pipe manager). */
    static unsigned s_next_pipe_uid = 1;  /* 0 reserved; pipe mgr starts at 1 */
    unsigned uid = s_next_pipe_uid++;

    seL4_Word read_badge  = ((seL4_Word)uid << 1) | 0u;
    seL4_Word write_badge = ((seL4_Word)uid << 1) | 1u;

    seL4_CPtr read_slot  = tm_process_alloc_slot(caller);
    seL4_CPtr write_slot = tm_process_alloc_slot(caller);
    seL4_Uint8 depth = cnode_depth_for(caller);

    if (qsoe_cnode_mint(client->cnode, read_slot, depth,
                        s_cnode_root, master, TM_DEPTH_TASKMAN,
                        QSOE_RIGHTS_SEND, read_badge) != 0) {
        return -ENOMEM;
    }
    if (qsoe_cnode_mint(client->cnode, write_slot, depth,
                        s_cnode_root, master, TM_DEPTH_TASKMAN,
                        QSOE_RIGHTS_SEND, write_badge) != 0) {
        /* read-end already minted; not unwinding for v0.7 — caller
         * sees ENOMEM and the read-end leaks until process exit
         * cleans up the whole CSpace. */
        return -ENOMEM;
    }

    /* Register both connections in taskman's table so subsequent
     * tm_connect_detach (libc close) on either fd works.  flags=0
     * (no COF_* bits for pipe ends in v0.7). */
    tm_connection_register_existing(caller, read_slot,  chidx, read_badge,  0);
    tm_connection_register_existing(caller, write_slot, chidx, write_badge, 0);

    *out_read_slot  = read_slot;
    *out_write_slot = write_slot;
    return 0;
}
