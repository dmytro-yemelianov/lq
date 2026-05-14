/*
 * cpiofs.c — read-only filesystem over the embedded userland CPIO.
 * See cpiofs.h for the design rationale.
 */

#include "cpiofs.h"
#include "sel4_syscalls.h"
#include "server.h"
#include "../libqsoe/include/qsoe/qrv.h"
#include <cpio/cpio.h>

/* Set at boot from main.c — same CPIO blob taskman uses for spawn. */
static const void *s_cpio_start;
static unsigned long s_cpio_len;

void tm_cpiofs_set_cpio(const void *start, unsigned long len);
void tm_cpiofs_set_cpio(const void *start, unsigned long len)
{
    s_cpio_start = start;
    s_cpio_len   = len;
}

int tm_cpiofs_open(const char *open_path, unsigned consumed,
                   seL4_Word badge)
{
    if (!s_cpio_start) return -EINVAL;
    if (!open_path)    return -EINVAL;

    /* Skip the leading bytes the prefix-match consumed (typically the
     * sole '/'), then skip any further slashes. cpio_get_file wants
     * "bin/hello.elf", not "/bin/hello.elf". */
    const char *name = open_path + consumed;
    while (*name == '/') ++name;
    if (*name == 0) return -EISDIR;  /* opening the root directory itself */

    unsigned long size = 0;
    const void *data = cpio_get_file(s_cpio_start, s_cpio_len, name, &size);
    if (!data) return -ENOENT;
    if (size > 0xFFFFFFFFul) return -EFBIG;  /* 4 GiB cap on file size */

    /* Stash (data_ptr, packed(offset=0, size)) on the connection.
     * Subsequent reads decode via the bit-packing in tm_cpiofs_read. */
    unsigned long packed = ((unsigned long)size) << 32;  /* offset=0 in low 32 */
    return tm_connection_set_ctx(badge, (unsigned long)data, packed);
}

/* IPC-buffer payload area capacity, mirroring the limit in
 * libqsoe/src/io.c. The kernel transfers msg[4..length-1] across
 * an IPC, with MR0..3 register-passed. */
#define TM_CPIOFS_MAX_CHUNK 928u

int tm_cpiofs_read(seL4_Word badge, unsigned want, unsigned *got)
{
    *got = 0;
    if (!s_cpio_start) return -EINVAL;

    unsigned long data_addr = 0;
    unsigned long offset    = 0;
    int rc = tm_connection_get_ctx(badge, &data_addr, &offset);
    if (rc) return rc;
    if (data_addr == 0) return -EBADF;

    /* We don't have the file size on hand here — we stored only the
     * data pointer. cpio_get_file re-walking is wasteful per-read.
     * Workaround for v0.6.0: re-derive the size by walking the CPIO
     * entry header backwards isn't trivial either. Instead, the
     * cleanest fix is to also stash the size; we do it lazily.
     *
     * For v0.6.0, we cap reads by the IPC chunk size and let the
     * client loop until they get a short read of 0 bytes. We detect
     * EOF by re-querying cpio_get_file against the original name —
     * we don't have the name. Compromise: store size in the high
     * 32 bits of offset (cap files at 4 GiB, fine).
     *
     * Actually re-thinking: cpio_get_file gave us size at open; we
     * stash it at open time but we also stash data ptr. ctx[] only
     * has two slots. We use ctx[0]=data_ptr, ctx[1] packs
     * offset(low32) | size(high32). That gives 4 GiB max file
     * size, plenty. */

    unsigned long packed = offset;
    unsigned long off  = packed & 0xFFFFFFFFu;
    unsigned long size = packed >> 32;

    if (off >= size) return 0;  /* EOF — *got stays 0 */

    unsigned remaining = (unsigned)(size - off);
    unsigned chunk = want;
    if (chunk > TM_CPIOFS_MAX_CHUNK) chunk = TM_CPIOFS_MAX_CHUNK;
    if (chunk > remaining)            chunk = remaining;

    const unsigned char *src = (const unsigned char *)(data_addr + off);
    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < chunk; ++i) dst[i] = src[i];

    off += chunk;
    rc = tm_connection_set_ctx(badge, data_addr,
                               (off & 0xFFFFFFFFu) | (size << 32));
    if (rc) return rc;

    *got = chunk;
    return 0;
}
