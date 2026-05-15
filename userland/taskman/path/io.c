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
#include "../sys/console.h"
#include "../proc/proc.h"
#include "../qsoe_invoke.h"
#include "../../libqsoe/include/qsoe/wire.h"

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
    }
    *out_slot = slot;
    return 0;
}

int tm_io_close(pid_t caller, seL4_CPtr slot)
{
    return tm_connect_detach(caller, slot);
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
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
        return tm_cpiofs_read(badge, want, out_got);
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
    if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CONSOLE) {
        /* /dev/console exists, but it's a device node — unlinking
         * it has no meaning under QSOE.  POSIX-y reply: EPERM. */
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
    if (srv_pid == QSOE_PID_TASKMAN && srv_chid == TM_CPIOFS_CHID) {
        int rc = tm_cpiofs_stat(badge, out);
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
    if (obj.handler_kind == PATHMGR_HANDLER_TASKMAN_CONSOLE) {
        return -EINVAL;                    /* /dev/console isn't a symlink */
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
