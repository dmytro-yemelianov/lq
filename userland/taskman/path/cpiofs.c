/*
 * cpiofs.c — read-only filesystem over the embedded userland CPIO.
 * See cpiofs.h for the design rationale.
 */

#include "cpiofs.h"
#include "../sel4_syscalls.h"
#include "../proc/proc.h"
#include "../../libqsoe/include/qsoe/qrv.h"
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

/* Per-directory iterator state.  Allocated on opendir, freed on the
 * matching close (well, leaked for v0.7 since we don't yet plumb a
 * cpiofs-side close hook; the table is bounded so this self-throttles
 * to TM_CPIOFS_MAX_DIRS open directories simultaneously). */
#define TM_CPIOFS_MAX_DIRS 16
static struct {
    seL4_Word badge;       /* 0 = unused */
    char      prefix[64];  /* e.g. "" (root) or "bin/" — incl. trailing '/' */
    unsigned  prefix_len;
    unsigned  next_idx;    /* next CPIO entry index to inspect */
    char      last_subdir[64];   /* last subdir name returned (dedup) */
} g_cpiofs_dirs[TM_CPIOFS_MAX_DIRS];

static unsigned q_strlen(const char *s)
{
    unsigned n = 0; while (s[n]) ++n; return n;
}

static int dir_slot_alloc(seL4_Word badge, const char *prefix)
{
    for (int i = 0; i < TM_CPIOFS_MAX_DIRS; ++i) {
        if (g_cpiofs_dirs[i].badge == 0) {
            unsigned len = q_strlen(prefix);
            if (len >= sizeof g_cpiofs_dirs[i].prefix) return -ENAMETOOLONG;
            for (unsigned k = 0; k <= len; ++k) {
                g_cpiofs_dirs[i].prefix[k] = prefix[k];
            }
            g_cpiofs_dirs[i].prefix_len     = len;
            g_cpiofs_dirs[i].next_idx       = 0;
            g_cpiofs_dirs[i].last_subdir[0] = 0;
            g_cpiofs_dirs[i].badge          = badge;
            return i;
        }
    }
    return -EMFILE;
}

/* Does any cpio entry start with `name + "/"`?  Used at open time to
 * decide whether a non-file path is actually a directory. */
static int cpiofs_is_subdir(const char *name)
{
    unsigned name_len = q_strlen(name);
    struct cpio_info info;
    if (cpio_info(s_cpio_start, s_cpio_len, &info) != 0) return 0;
    for (unsigned i = 0; i < info.file_count; ++i) {
        const char *ent_name = 0;
        unsigned long ent_size = 0;
        const void *ent = cpio_get_entry(s_cpio_start, s_cpio_len,
                                          (int)i, &ent_name, &ent_size);
        if (!ent || !ent_name) continue;
        /* Match iff ent_name == name + "/..." */
        int match = 1;
        for (unsigned k = 0; k < name_len; ++k) {
            if (ent_name[k] != name[k]) { match = 0; break; }
        }
        if (match && ent_name[name_len] == '/') return 1;
    }
    return 0;
}

int tm_cpiofs_open(const char *open_path, unsigned consumed,
                   seL4_Word badge)
{
    if (!s_cpio_start) return -EINVAL;
    if (!open_path)    return -EINVAL;

    /* Skip the prefix the pathmgr already consumed, then any extras. */
    const char *name = open_path + consumed;
    while (*name == '/') ++name;

    if (*name == 0) {
        /* Root directory — every "bin/..." entry hangs off here. */
        int idx = dir_slot_alloc(badge, "");
        if (idx < 0) return idx;
        /* ctx[0] = 0 marks a directory; ctx[1] = dir-table index. */
        return tm_connection_set_ctx(badge, 0, (unsigned long)idx);
    }

    /* Regular file path. */
    unsigned long size = 0;
    const void *data = cpio_get_file(s_cpio_start, s_cpio_len, name, &size);
    if (data) {
        if (size > 0xFFFFFFFFul) return -EFBIG;
        unsigned long packed = ((unsigned long)size) << 32;
        return tm_connection_set_ctx(badge, (unsigned long)data, packed);
    }

    /* Not a file — is it the prefix of some entry?  Then it's a dir. */
    if (cpiofs_is_subdir(name)) {
        char prefix[64];
        unsigned len = q_strlen(name);
        if (len + 2 > sizeof prefix) return -ENAMETOOLONG;
        for (unsigned i = 0; i < len; ++i) prefix[i] = name[i];
        prefix[len]     = '/';
        prefix[len + 1] = 0;
        int idx = dir_slot_alloc(badge, prefix);
        if (idx < 0) return idx;
        return tm_connection_set_ctx(badge, 0, (unsigned long)idx);
    }

    return -ENOENT;
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

int tm_cpiofs_stat(seL4_Word badge, tm_stat_t *out)
{
    unsigned long data_addr = 0;
    unsigned long packed    = 0;
    int rc = tm_connection_get_ctx(badge, &data_addr, &packed);
    if (rc) return rc;

    /* Zero the whole struct first so anything we don't set reads as
     * zero on the client side. */
    unsigned char *p = (unsigned char *)out;
    for (unsigned i = 0; i < sizeof *out; ++i) p[i] = 0;

    if (data_addr == 0) {
        /* Directory: ctx[1] holds the dirs-table index. */
        out->st_dev     = 1;
        out->st_ino     = 0x10000 + packed;   /* synthetic dir inode */
        out->st_mode    = TM_S_IFDIR | 0555;  /* read-only directory */
        out->st_nlink   = 2;
        out->st_size    = 0;
        out->st_blksize = 512;
        return 0;
    }

    unsigned long size = packed >> 32;

    out->st_dev     = 1;             /* synthetic dev for cpiofs */
    out->st_ino     = data_addr;     /* CPIO data ptr is a stable id */
    out->st_mode    = TM_S_IFREG | 0444;  /* read-only regular file */
    out->st_nlink   = 1;
    out->st_uid     = 0;
    out->st_gid     = 0;
    out->st_size    = (long)size;
    out->st_blksize = 512;
    out->st_blocks  = (long)((size + 511) / 512);
    return 0;
}

int tm_cpiofs_probe(const char *name)
{
    if (!s_cpio_start) return -ENOENT;
    if (!name || *name == 0) return -ENOENT;
    unsigned long size = 0;
    const void *data = cpio_get_file(s_cpio_start, s_cpio_len, name, &size);
    return data ? 0 : -ENOENT;
}

int tm_cpiofs_lseek(seL4_Word badge, int whence, long offset, long *out_off)
{
    unsigned long data_addr = 0;
    unsigned long packed    = 0;
    int rc = tm_connection_get_ctx(badge, &data_addr, &packed);
    if (rc) return rc;
    if (data_addr == 0) return -EBADF;

    unsigned long off  = packed & 0xFFFFFFFFu;
    unsigned long size = packed >> 32;
    long new_off;

    switch (whence) {
    case 0:  new_off = offset; break;                     /* SEEK_SET */
    case 1:  new_off = (long)off + offset; break;         /* SEEK_CUR */
    case 2:  new_off = (long)size + offset; break;        /* SEEK_END */
    default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    /* POSIX permits seeking past EOF on a regular file; cpiofs is
     * read-only so the next read will just return 0 bytes (EOF). */
    if ((unsigned long)new_off > 0xFFFFFFFFul) return -EINVAL;

    rc = tm_connection_set_ctx(badge, data_addr,
                               ((unsigned long)new_off & 0xFFFFFFFFu) |
                               (size << 32));
    if (rc) return rc;
    *out_off = new_off;
    return 0;
}

int tm_cpiofs_readdir(seL4_Word badge, char *name_out,
                      unsigned *namelen_out, int *d_type_out)
{
    *namelen_out = 0;
    if (!s_cpio_start) return -EINVAL;

    unsigned long data_addr = 0, ctx1 = 0;
    int rc = tm_connection_get_ctx(badge, &data_addr, &ctx1);
    if (rc) return rc;
    if (data_addr != 0) return -ENOTDIR;   /* ctx[0]!=0 means file */

    int slot_idx = (int)ctx1;
    if (slot_idx < 0 || slot_idx >= TM_CPIOFS_MAX_DIRS) return -EBADF;
    if (g_cpiofs_dirs[slot_idx].badge != badge) return -EBADF;

    struct cpio_info info;
    if (cpio_info(s_cpio_start, s_cpio_len, &info) != 0) return -EIO;

    unsigned plen = g_cpiofs_dirs[slot_idx].prefix_len;
    const char *prefix = g_cpiofs_dirs[slot_idx].prefix;

    while (g_cpiofs_dirs[slot_idx].next_idx < info.file_count) {
        const char *ent_name = 0;
        unsigned long ent_size = 0;
        const void *ent = cpio_get_entry(s_cpio_start, s_cpio_len,
                                          (int)g_cpiofs_dirs[slot_idx].next_idx,
                                          &ent_name, &ent_size);
        g_cpiofs_dirs[slot_idx].next_idx++;
        if (!ent || !ent_name) continue;

        /* Filter: name must start with our prefix. */
        int match = 1;
        for (unsigned k = 0; k < plen; ++k) {
            if (ent_name[k] != prefix[k]) { match = 0; break; }
        }
        if (!match) continue;

        const char *rel = ent_name + plen;
        if (*rel == 0) continue;   /* the prefix itself, skip */

        /* Find first '/' in rel — splits subdir from filename. */
        unsigned sep = 0;
        while (rel[sep] && rel[sep] != '/') ++sep;

        if (rel[sep] == '/') {
            /* Subdirectory entry — dedup against last_subdir. */
            int same = 1;
            for (unsigned k = 0; k < sep; ++k) {
                if (g_cpiofs_dirs[slot_idx].last_subdir[k] != rel[k]) {
                    same = 0; break;
                }
            }
            if (same && g_cpiofs_dirs[slot_idx].last_subdir[sep] == 0) {
                /* Already returned this subdir — skip. */
                continue;
            }
            if (sep >= sizeof g_cpiofs_dirs[slot_idx].last_subdir) {
                continue;  /* name too long; skip rather than truncate */
            }
            for (unsigned k = 0; k < sep; ++k) {
                g_cpiofs_dirs[slot_idx].last_subdir[k] = rel[k];
                name_out[k] = rel[k];
            }
            g_cpiofs_dirs[slot_idx].last_subdir[sep] = 0;
            name_out[sep] = 0;
            *namelen_out = sep;
            *d_type_out  = 4;  /* DT_DIR */
            return 0;
        } else {
            /* File entry — full rel is the basename. */
            unsigned rlen = sep;  /* sep == strlen(rel) here */
            if (rlen >= 256) continue;  /* dirent d_name caps at 256 */
            for (unsigned k = 0; k < rlen; ++k) name_out[k] = rel[k];
            name_out[rlen] = 0;
            *namelen_out = rlen;
            *d_type_out  = 8;  /* DT_REG */
            return 0;
        }
    }
    return -ENOENT;
}
