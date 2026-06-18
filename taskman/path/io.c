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
#include <tm_cpio.h>
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
int tm_io_open(pid_t caller, unsigned path_len, seL4_CPtr *out_slot,
               int *out_is_external, unsigned *out_rwlen)
{
    if (out_is_external) *out_is_external = 0;
    if (out_rwlen) *out_rwlen = 0;
    if (path_len == 0 || path_len >= 128) return -EINVAL;

    static char s_open_path[128];
    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < path_len; ++i) s_open_path[i] = (char)src[i];
    s_open_path[path_len] = 0;

    /* Expand a leading cross-fs symlink (/etc -> /usr/conf, /home ->
     * /usr/home) so the resolve lands on the right server AND the external
     * resmgr (fs-qrv) receives the rewritten path on the _IO_CONNECT libc
     * sends next: the server keys on the path string, so it must see
     * /usr/conf/passwd, not /etc/passwd.  One level only. */
    static char s_eff[128];
    const char *eff = s_open_path;
    /* First the in-memory tree (virtual links like /dev/tty), then the
     * cpio (real link inodes /etc -> /usr/conf, /home -> /usr/home). */
    if (tm_pathmgr_expand_symlink(s_open_path, s_eff, sizeof s_eff) == 1) {
        eff = s_eff;
    } else {
        const void   *cpio = 0;
        unsigned long clen = 0;
        tm_cpiofs_get_cpio(&cpio, &clen);
        if (tm_pathmgr_expand_symlink_cpio((const uint8_t *)cpio, clen,
                                           s_open_path, s_eff,
                                           sizeof s_eff) == 1)
            eff = s_eff;
    }

    tm_pathmgr_obj_t obj;
    unsigned consumed = 0;
    int rc = tm_pathmgr_resolve(eff, &obj, &consumed);
    if (rc) return rc;

    /* ConnectAttach mints a badged Send cap on (server_pid, server_chid). */
    seL4_CPtr slot = 0;
    rc = tm_connect_attach(caller, obj.server_pid, obj.server_chid, 0, &slot);
    if (rc) return rc;

    /* An external resmgr (a libressrv server, not one of taskman's own
     * synthetic handlers) creates its per-open handle on _IO_CONNECT.
     * taskman minted the cap but can't run the server's acquire(); tell
     * libc so it sends _IO_CONNECT on the fd before the first read. */
    if (out_is_external)
        *out_is_external = (obj.handler_kind == PATHMGR_HANDLER_EXTERNAL);

    /* If a symlink fired, return the rewritten path to libc (in the reply's
     * msg[4..]); libc uses it as the _IO_CONNECT path to the resmgr. */
    if (eff != s_open_path && out_rwlen) {
        unsigned elen = 0; while (eff[elen]) ++elen;
        unsigned char *rdst = (unsigned char *)&qsoe_ipcbuf->msg[4];
        for (unsigned i = 0; i < elen; ++i) rdst[i] = (unsigned char)eff[i];
        *out_rwlen = elen;
    }

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

/* READLINK: symlinks live in the boot cpio -- the cross-fs mount links
 * (/etc -> /usr/conf, /home -> /usr/home) and bin/sh -> qsh.  Look the
 * entry up WITHOUT following it (the shared find_file is exact-match, not
 * resolve): a symlink replies its target, a non-symlink EINVAL, a miss
 * ENOENT.  The pathmgr tree carries no symlink for these now -- the cpio
 * inode is the single source of truth. */
#define TM_READLINK_PATH_MAX 128   /* path + target reply buffer cap (bytes) */
int tm_readlink(pid_t caller, unsigned path_len, unsigned *out_bytes)
{
    (void)caller;
    *out_bytes = 0;
    if (path_len == 0 || path_len >= TM_READLINK_PATH_MAX) return -EINVAL;

    /* Pure-payload frame: the path argument rides contiguously right after
     * the header word, i.e. at reply/request word 1 (msg[1..]).  The
     * dispatcher mirrored MR0..3 into msg[0..3] on entry, so msg[1..] is
     * the full path even for the part that arrived in registers. */
    static char s_path[TM_READLINK_PATH_MAX];
    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[1];
    for (unsigned i = 0; i < path_len; ++i) s_path[i] = (char)src[i];
    s_path[path_len] = 0;

    const void   *cpio = 0;
    unsigned long clen = 0;
    tm_cpiofs_get_cpio(&cpio, &clen);
    if (!cpio || clen == 0) return -ENOENT;

    /* cpio names are slash-free at the root ("etc", not "/etc"); a caller
     * may pass "//etc" (ls joins "/" + "etc"), so skip ALL leading slashes. */
    const char *name = s_path;
    while (*name == '/') ++name;
    if (*name == 0) return -EINVAL;          /* "/" is a dir, not a link */

    tm_cpio_file_info_t info;
    if (!tm_cpio_find_file((const uint8_t *)cpio, clen, name, &info))
        return -ENOENT;
    if ((info.mode & TM_CPIO_S_IFMT) != TM_CPIO_S_IFLNK)
        return -EINVAL;                      /* exists but not a symlink */

    /* Target = the entry data (filesize bytes, no NUL in the archive).
     * Write it at reply word 1 (msg[1..]) so the reply is contiguous --
     * length at word 0, target from word 1.  The READLINK dispatch case
     * lifts msg[1..3] into the reply's MR1..3. */
    unsigned tlen = info.filesize;
    if (tlen >= TM_READLINK_PATH_MAX) tlen = TM_READLINK_PATH_MAX - 1;
    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[1];
    for (unsigned i = 0; i < tlen; ++i) dst[i] = info.data[i];
    *out_bytes = tlen;
    return 0;
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
/* On-wire directory record: byte-identical to libc's struct dirent
 * (libc/include/bits/dirent.h).  taskman is -nostdinc, so we mirror the
 * layout here -- the libc readdir() client casts the reply bytes straight
 * to struct dirent, so fields and offsets must match exactly.  This is the
 * libressrv _IO_READDIR framing (fs-qrv emits the same records), so the one
 * client speaks to both taskman's internal dirs and external resmgrs. */
#define TM_DIRENT_NAME_MAX 256   /* d_name[] -- matches libc struct dirent  */
#define TM_DIRENT_ALIGN      8   /* record stride == sizeof(off_t); keeps   */
                                 /* each record's d_ino/d_off 8-byte aligned */

struct tm_dirent {
    unsigned long  d_ino;          /* ino_t  */
    long           d_off;          /* off_t  */
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[TM_DIRENT_NAME_MAX];
};

/* Pack one entry as a struct dirent record at p; return its byte length.
 * d_ino/d_off are synthetic monotonic values -- POSIX requires neither
 * stability nor uniqueness for these read-only listings, only d_ino != 0
 * (a zero inode reads as a deleted slot to some getdents consumers). */
static unsigned pack_dirent(unsigned char *p, int d_type,
                            const char *name, unsigned namelen)
{
    static unsigned long s_seq;          /* synthetic d_ino/d_off source */
    unsigned hdr = (unsigned)__builtin_offsetof(struct tm_dirent, d_name);
    unsigned reclen = hdr + namelen + 1;   /* + NUL terminator */
    reclen = (reclen + (TM_DIRENT_ALIGN - 1)) & ~(unsigned)(TM_DIRENT_ALIGN - 1);

    for (unsigned i = 0; i < reclen; ++i) p[i] = 0;   /* no stale padding */
    struct tm_dirent *de = (struct tm_dirent *)p;
    ++s_seq;
    de->d_ino    = s_seq;
    de->d_off    = (long)s_seq;
    de->d_reclen = (unsigned short)reclen;
    de->d_type   = (unsigned char)d_type;
    for (unsigned i = 0; i < namelen; ++i) de->d_name[i] = name[i];
    de->d_name[namelen] = 0;
    return reclen;
}

int tm_readdir(pid_t caller, seL4_Word badge, unsigned *out_bytes)
{
    (void)caller;
    pid_t srv_pid = 0;
    int   srv_chid = 0;
    if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) return -EBADF;
    if (srv_pid != QSOE_PID_TASKMAN) return -ENOSYS;

    char name[TM_DIRENT_NAME_MAX];
    unsigned namelen = 0;
    int d_type = 0;
    int rc;

    /* Each per-kind handler yields one entry per call (cursor in the
     * connection ctx).  We frame that single entry as a struct dirent
     * record; the buffered client refills once per entry.  fs-qrv packs
     * many records per reply -- same record format, same client. */
    switch (srv_chid) {
    case TM_CONSOLE_CHID:
        return -ENOTDIR;
    case TM_CPIOFS_CHID:
        rc = tm_cpiofs_readdir(badge, name, &namelen, &d_type); break;
    case TM_PMDIR_CHID:
        rc = tm_pmdir_readdir(badge, name, &namelen, &d_type); break;
    case TM_SYSFS_CHID:
        rc = tm_sysfs_readdir(badge, name, &namelen, &d_type); break;
    case TM_PROCFS_CHID:
        rc = tm_procfs_readdir(badge, name, &namelen, &d_type); break;
    default:
        return -ENOSYS;
    }
    if (rc) return rc;
    if (namelen >= TM_DIRENT_NAME_MAX) namelen = TM_DIRENT_NAME_MAX - 1;

    *out_bytes = pack_dirent((unsigned char *)&qsoe_ipcbuf->msg[4],
                             d_type, name, namelen);
    return 0;
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
