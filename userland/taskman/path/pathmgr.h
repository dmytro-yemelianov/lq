/*
 * pathmgr.h — taskman's path namespace registry (v0.5+).
 *
 * Prefix-tree of nodes, one node per path component. Each node may
 * carry an attached "object" identifying which server (pid, chid)
 * handles requests on that prefix. Lookup is longest-prefix:
 * tm_pathmgr_resolve walks the tree component-by-component and
 * returns the deepest node with an attached object plus the number
 * of path bytes that matched (so future resmgrs can serve subtrees).
 *
 * Inspired by QRV's tNode (~/proj/QRV-OS/taskman/include/pathmgr_node.h)
 * but written from scratch — fixed-pool storage, no reference counting
 * (yet), no symlinks (yet). v0.5.0 holds a handful of entries; the
 * data structure scales to hundreds before lookup gets noticeable.
 */
#ifndef QSOE_TASKMAN_PATHMGR_H
#define QSOE_TASKMAN_PATHMGR_H

#include "../sel4_types.h"
#include <qsoe-system.h>

/* Handler kinds. v0.5.0 uses these to route in-taskman traffic
 * without a real ConnectAttach hop; v0.6+ external resmgrs always
 * use HANDLER_EXTERNAL and resolve via (server_pid, server_chid). */
#define PATHMGR_HANDLER_EXTERNAL        0
#define PATHMGR_HANDLER_TASKMAN_CONSOLE 1
#define PATHMGR_HANDLER_TASKMAN_CPIOFS  2  /* v0.6.0 */
#define PATHMGR_HANDLER_TASKMAN_NULL    3  /* v0.8.0 */
#define PATHMGR_HANDLER_TASKMAN_ZERO    4  /* v0.8.0 */
#define PATHMGR_HANDLER_TASKMAN_PMDIR   5  /* synthetic dir backed by
                                            * the pathmgr's own tree —
                                            * used for /dev so ls(1)
                                            * can enumerate registered
                                            * device nodes */

/* Internal taskman channel ids beyond the primary (1). All share the
 * primary endpoint; (TASKMAN_PID, *_CHID) is registered in the
 * channel table so ConnectAttach can mint badged Send caps onto
 * them, and the dispatch loop routes IO_WRITE/IO_READ by checking
 * which channel each badge points at. */
#define TM_CONSOLE_CHID  2
#define TM_CPIOFS_CHID   3
#define TM_DEVNULL_CHID  4
#define TM_DEVZERO_CHID  5
#define TM_PMDIR_CHID    6   /* synthetic pathmgr directories (/dev) */

typedef struct tm_pathmgr_obj {
    pid_t    server_pid;
    int      server_chid;
    unsigned flags;
    unsigned handler_kind;
} tm_pathmgr_obj_t;

/* Initialise registry. Allocates the root node. Call once at boot. */
void tm_pathmgr_init(void);

/* Register obj at the given absolute path (must start with '/'). The
 * tree is grown on demand. Returns 0 on success, -errno on failure
 * (-EINVAL bad path, -ENOMEM pool exhausted, -EEXIST already taken). */
int tm_pathmgr_register(const char *path, const tm_pathmgr_obj_t *obj);

/* Longest-prefix lookup. On match, fills *out with the deepest
 * matching object and *out_consumed_bytes with the number of bytes
 * in `path` that the prefix covered. Returns 0 on match, -ENOENT
 * if no node along the path carried an object. */
int tm_pathmgr_resolve(const char *path,
                       tm_pathmgr_obj_t *out,
                       unsigned *out_consumed_bytes);

/* v0.6.1: update an existing path's attached object to point at a
 * different server. Used to swap /dev/console between handlers
 * after a real driver comes up. Returns 0 on success, -ENOENT if
 * the path doesn't exist, -EINVAL on bad input. The path must
 * already be registered; this isn't a create-or-update. */
int tm_pathmgr_repath(const char *path, const tm_pathmgr_obj_t *new_obj);

/* v0.8: create a pathmgr symlink.  After this call, resolving
 * `link_path` walks the registered target_path and returns whatever
 * IT resolves to — i.e. a symlink to /dev/console follows the
 * console wherever it's repath'd.  Maximum one redirection per
 * resolve (no chained symlinks for v0.8).  Returns 0 on success,
 * -EINVAL on bad input, -ENOMEM if the pool is full, -EEXIST if
 * link_path is already registered with a different attachment. */
int tm_pathmgr_symlink(const char *link_path, const char *target_path);

/* Enumerate direct children of the pathmgr node at `path`.  `idx` is
 * zero-based; on a match the child's name (NUL-terminated) is
 * written to `name_out` (caller-sized; clamped to cap) and *namelen
 * gets its length.  Returns 0 on hit, -ENOENT past the end, -EINVAL
 * for a bad path / unknown node.  Used by tm_pmdir_readdir (for
 * /dev) and by tm_cpiofs_readdir (to merge pathmgr children of "/"
 * with CPIO entries so ls / sees "dev" alongside "bin", "sbin"). */
int tm_pathmgr_child_at(const char *path, unsigned idx,
                        char *name_out, unsigned name_cap,
                        unsigned *out_namelen);

#endif /* QSOE_TASKMAN_PATHMGR_H */
