/*
 * cpiofs.h — read-only filesystem backed by the embedded userland CPIO.
 *
 * Registered in the path manager at "/" with handler_kind=CPIOFS at
 * boot. open("/bin/qsh") consults the path manager (longest-
 * prefix match), gets routed here, the leading "/" is stripped, and
 * the remainder is looked up in the in-memory CPIO archive via
 * cpio_get_file. The resulting (data, size) is stashed in the
 * connection's per-fd ctx[] so subsequent reads can resume from the
 * right offset.
 *
 * v0.6.0: read-only. Writes return -EROFS. Per-fd seek isn't exposed
 * via TM_REQ_* yet but the offset state is in place — a future
 * TM_REQ_IO_SEEK can land it cheaply.
 */
#ifndef QSOE_TASKMAN_CPIOFS_H
#define QSOE_TASKMAN_CPIOFS_H

#include "../sel4_types.h"
#include "path.h"

/* Boot-time setup: hand cpiofs the embedded CPIO blob. */
void tm_cpiofs_set_cpio(const void *start, unsigned long len);

/* Look up `name` in the CPIO archive with one level of symlink
 * resolution.  `name` is unprefixed (e.g. "bin/sh", not "/bin/sh").
 * If the entry is a symlink (S_IFLNK mode), its target is resolved
 * once: absolute targets ("/...") have the leading '/' stripped,
 * relative targets are joined with the link's parent directory.
 * Chained symlinks (target is also a symlink) return NULL.
 * Used by tm_cpiofs_* and by spawn.c (shebang interpreter lookup). */
const void *tm_cpio_lookup(const char *name, unsigned long *out_size);

/* Called from main.c's TM_REQ_OPEN handler when the resolved
 * pathmgr object has handler_kind=PATHMGR_HANDLER_TASKMAN_CPIOFS.
 *
 *   open_path: full path the client passed (e.g. "/bin/qsh")
 *   consumed:  bytes of path the prefix-match consumed (1 for "/")
 *   badge:     scoid of the freshly-minted connection (set by the
 *              caller before calling this — we stash file state on it)
 *
 * Returns 0 on success, -errno on lookup failure. The caller has
 * already minted the Send cap and registered the connection.
 */
int tm_cpiofs_open(const char *open_path, unsigned consumed,
                   seL4_Word badge);

/* Handle TM_REQ_IO_READ on a cpiofs-bound connection.
 *   badge: arrived scoid (identifies which file)
 *   want:  byte count the client wants (capped at IPC payload limit)
 * On success returns 0 and writes the byte count into *got;
 * the bytes are placed in qsoe_ipcbuf->msg[4..]. On EOF returns
 * 0 with *got=0. */
int tm_cpiofs_read(seL4_Word badge, unsigned want, unsigned *got);

/* Fill *out with file metadata for the cpiofs-bound connection at
 * `badge`.  Reads file size from the connection's stashed ctx[]
 * (set at open).  Returns 0 / -EBADF.  Mode is S_IFREG with the
 * usual rwxr-xr-x permissions; cpiofs has no per-file ownership. */
int tm_cpiofs_stat(seL4_Word badge, tm_stat_t *out);

/* Probe whether `name` exists in the CPIO archive (used by unlink
 * to distinguish ENOENT from EROFS).  Returns 0 if it exists,
 * -ENOENT otherwise.  Name is relative — leading slashes stripped
 * by the caller exactly as in tm_cpiofs_open. */
int tm_cpiofs_probe(const char *name);

/* Reposition the read pointer on a cpiofs connection.
 *   whence: 0=SET, 1=CUR, 2=END
 *   offset: signed byte offset
 * Writes the resulting absolute offset into *out_off.  Returns
 * 0 / -EBADF / -EINVAL. */
int tm_cpiofs_lseek(seL4_Word badge, int whence, long offset, long *out_off);

/* Return the next dirent for a connection opened on a directory.
 *
 *   badge:    the connection's scoid
 *   name:     output, NUL-terminated entry name written here
 *   namelen:  output, length of name (excluding NUL)
 *   d_type:   output, DT_REG / DT_DIR
 *
 * Returns 0 on success, -ENOENT past the end of the directory,
 * -ENOTDIR if the connection was opened on a regular file. */
int tm_cpiofs_readdir(seL4_Word badge, char *name, unsigned *namelen,
                      int *d_type);

/* Per-fd close hook.  Frees the dir-slot entry tied to `badge` if
 * one exists; no-op for regular-file connections (their per-fd
 * state lives entirely in the connection-context slots that taskman
 * already clears on tm_connect_detach).  Returns 0 either way. */
int tm_cpiofs_close(seL4_Word badge);

#endif
