/*
 * readdir.c — POSIX readdir().
 *
 * Sends TM_REQ_READDIR via the DIR's fd, parses the reply payload
 * into a static `struct dirent`, returns &it.  POSIX permits a
 * single per-DIR slot reused on each call; v0.7 isn't multi-
 * threaded inside a process so a file-scope static is fine — when
 * we grow real threads readdir_r is the MT-safe variant.
 */

#include <dirent.h>
#include <qsoe/qrv.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"
#include "__dirent.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

static struct dirent s_ent;

struct dirent *readdir(DIR *d)
{
    if (!d) { qsoe_errno = EINVAL; return 0; }
    seL4_CPtr slot = (seL4_CPtr)qsoe_state_coid_to_slot(d->fd);
    if (!slot) { qsoe_errno = EBADF; return 0; }

    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_READDIR, 0, 0, 0);
    seL4_MessageInfo_t reply = qsoe_sys_call(slot, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) {
        /* ENOENT = end-of-directory.  POSIX: return NULL, errno
         * unchanged.  Anything else: NULL + errno set. */
        if (err != (seL4_Word)ENOENT) qsoe_errno = (int)err;
        return 0;
    }

    unsigned bytes = (unsigned)mr0;
    if (bytes < 2) { qsoe_errno = EIO; return 0; }   /* need d_type + NUL */
    const unsigned char *p = (const unsigned char *)&qsoe_ipcbuf->msg[4];

    /* Zero the dirent first so trailing bytes in d_name don't leak
     * stale data across calls. */
    unsigned char *dst = (unsigned char *)&s_ent;
    for (unsigned i = 0; i < sizeof s_ent; ++i) dst[i] = 0;

    s_ent.d_type = p[0];
    /* d_ino / d_off / d_reclen: synthesised — taskman doesn't track
     * stable inode numbers for cpiofs entries beyond the data ptr,
     * and POSIX doesn't require d_ino to be unique across hosts.
     * d->tell gets used by telldir(); we bump it monotonically. */
    s_ent.d_ino    = (++d->tell);
    s_ent.d_off    = d->tell;
    s_ent.d_reclen = (unsigned short)sizeof s_ent;

    /* Copy NUL-terminated name (bytes after the d_type byte) into
     * d_name.  d_name is 256 bytes; taskman bounds entry name to
     * the same limit. */
    unsigned i = 0;
    while (i + 1 < bytes && i < sizeof s_ent.d_name - 1) {
        s_ent.d_name[i] = (char)p[1 + i];
        if (p[1 + i] == 0) break;
        ++i;
    }
    s_ent.d_name[i] = 0;
    return &s_ent;
}
