/*
 * cpiofs.h — read-only filesystem backed by the embedded userland CPIO.
 *
 * Registered in the path manager at "/" with handler_kind=CPIOFS at
 * boot. open("/bin/hello.elf") consults the path manager (longest-
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

#include "sel4_types.h"

/* Boot-time setup: hand cpiofs the embedded CPIO blob. */
void tm_cpiofs_set_cpio(const void *start, unsigned long len);

/* Called from main.c's TM_REQ_OPEN handler when the resolved
 * pathmgr object has handler_kind=PATHMGR_HANDLER_TASKMAN_CPIOFS.
 *
 *   open_path: full path the client passed (e.g. "/bin/hello.elf")
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

#endif
