/*
 * path/path.h — Path Manager + IO dispatch.
 *
 * Public face of the OPEN / CLOSE / IO_WRITE / IO_READ wire ops
 * (each carved out of main.c's switch).  The handler functions
 * are called from main.c with the raw IPC arguments already
 * extracted; they reply via the out_* pointers.
 *
 * pathmgr.{c,h} and cpiofs.{c,h} sit alongside in this directory:
 *   - pathmgr  — path → (server pid, chid) registry + repath
 *   - cpiofs   — TM_REQ_OPEN backend for /bin (embedded CPIO)
 */
#ifndef QSOE_TASKMAN_PATH_H
#define QSOE_TASKMAN_PATH_H

#include "../sel4_types.h"
#include "../../libqsoe/include/qsoe/qrv.h"

/* POSIX `struct stat` byte-for-byte for RISC-V64 musl.  Built into the
 * fstat reply payload at msg[4..]; libc/qsoe's fstat.c copies the
 * bytes into the user-supplied struct stat.  Field types are spelled
 * out with concrete widths because taskman compiles -nostdinc and so
 * can't pull in musl's sys/types.h. */
typedef struct {
    unsigned long      st_dev;
    unsigned long      st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long      st_rdev;
    unsigned long      __pad;
    long               st_size;
    int                st_blksize;
    int                __pad2;
    long               st_blocks;
    long               st_atim_sec;
    long               st_atim_nsec;
    long               st_mtim_sec;
    long               st_mtim_nsec;
    long               st_ctim_sec;
    long               st_ctim_nsec;
    unsigned int       __unused[2];
} tm_stat_t;

/* Mode bits we need; matches POSIX / sys/stat.h. */
#define TM_S_IFMT   0170000
#define TM_S_IFREG  0100000
#define TM_S_IFCHR  0020000
#define TM_S_IFDIR  0040000

/* OPEN: open a path on behalf of `caller`.  path bytes start at
 * the IPC buffer's msg[4]; path_len in mr0.  Returns 0 + writes
 * the caller-side connection slot into *out_slot. */
int tm_io_open(pid_t caller, unsigned path_len, seL4_CPtr *out_slot);

/* CLOSE: tear down `caller`'s connection at the given slot. */
/* Resmgr-side close notification.  Dispatches by badge to the
 * owning resmgr's per-fd cleanup hook (cpiofs frees its dir-slot
 * entry; console has no per-fd state).  The cap itself is reclaimed
 * by a separate TM_REQ_DETACH_CAP call from libc — this function
 * does NOT touch the connection table or the caller's CSpace. */
int tm_io_close(pid_t caller, seL4_Word badge);

/* IO_WRITE: serve a write on `caller`'s `slot`.  bytes start at
 * msg[4]; bytecount in mr0.  Returns 0 + writes the bytes actually
 * written into *out_written. */
int tm_io_write(pid_t caller, seL4_Word badge, unsigned bytes,
                unsigned *out_written);

/* IO_READ: serve a read on `caller`'s `slot`.  Returns 0 + writes
 * bytes-read count into *out_got; payload is placed at msg[4..]. */
int tm_io_read(pid_t caller, seL4_Word badge, unsigned want,
               unsigned *out_got);

/* UNLINK: remove the named path.  Currently every QSOE resmgr is
 * read-only (cpiofs, console), so resolution is real but every
 * successful resolve answers EROFS / EPERM as appropriate.  When
 * a writable fs lands, this routes the request to its resmgr. */
int tm_unlink(pid_t caller, unsigned path_len);

/* FSTAT: fill a POSIX `struct stat` for the connection identified by
 * `badge`.  Bytes are written into msg[4..]; *out_bytes set to the
 * size of the stat record (callers know they expect a single struct
 * stat; the count goes back through MR0 for the IPC layer). */
int tm_fstat(pid_t caller, seL4_Word badge, unsigned *out_bytes);

/* READLINK: resolve the named path via pathmgr; if it points at a
 * symlink, write the link target to msg[4..] and set *out_bytes.
 * QSOE v0.7 has no symlinks, so a successful resolve yields EINVAL
 * (path exists but isn't a symlink), and a miss yields ENOENT. */
int tm_readlink(pid_t caller, unsigned path_len, unsigned *out_bytes);

/* LSEEK: reposition the fd's offset on the resmgr's connection.
 * `badge` identifies the connection; cpiofs tracks the offset in
 * its ctx[]; console / pipes return ESPIPE.  Writes the resulting
 * absolute offset into *out_off. */
int tm_lseek(pid_t caller, seL4_Word badge, int whence, long offset,
             long *out_off);

/* ACCESS: probe whether a path exists.  v0.7 ignores mode bits
 * (single-user root) — the only question that matters is whether
 * pathmgr resolves the path AND (for cpiofs) the file is present.
 * Returns 0 on success, -ENOENT on miss. */
int tm_access(pid_t caller, unsigned path_len);

/* PIPE_CREATE: mint two badged Send caps on /sbin/pipe's channel
 * into the caller's CSpace — one read end, one write end.  Badge
 * encoding shared with the pipe manager: bit 0 = direction
 * (0 = read, 1 = write); bits 1.. = monotonically-allocated unique
 * pipe id.  The pipe manager lazy-allocates a pool slot on first
 * IO using the unique id. */
int tm_pipe_create(pid_t caller, seL4_CPtr *out_read_slot,
                   seL4_CPtr *out_write_slot);

/* READDIR: fetch one directory entry from the connection at `badge`.
 *
 * Reply payload written to msg[4..]:
 *   [0]            d_type (1 byte: DT_REG=8, DT_DIR=4)
 *   [1..]          NUL-terminated d_name
 *
 * *out_bytes is set to (1 + name_len + 1), the total byte count in
 * msg[4..].  Label = ENOENT past the end of the directory; label =
 * ENOTDIR if the connection was opened on a regular file. */
int tm_readdir(pid_t caller, seL4_Word badge, unsigned *out_bytes);

#endif /* QSOE_TASKMAN_PATH_H */
