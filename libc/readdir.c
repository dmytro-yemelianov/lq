/*
 * readdir.c — POSIX readdir().
 *
 * Buffered getdents-style client, matching libressrv's _IO_READDIR
 * framing: each TM_REQ_READDIR reply carries one or more packed
 * `struct dirent` records (taskman's internal dirs emit one per reply;
 * fs-qrv packs many).  We cache a reply batch in the DIR's buf[] and
 * hand records out one at a time, refilling when it drains.  The records
 * are full struct dirent images, so we return a pointer straight into
 * the buffer — POSIX permits the returned storage to be reused on the
 * next call, and a single shared DIR is fine until real threads land
 * (readdir_r is the MT-safe variant then).
 */

#include <dirent.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include "state.h"
#include "__dirent.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

struct dirent *readdir(DIR *d)
{
    if (!d) { qsoe_errno = EINVAL; return 0; }

    if (d->buf_pos >= d->buf_end) {
        /* Buffer drained — fetch the next batch. */
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
        if (bytes == 0) return 0;                    /* end of directory */
        if (bytes > sizeof d->buf) bytes = sizeof d->buf;

        const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
        unsigned char *dst = (unsigned char *)d->buf;
        for (unsigned i = 0; i < bytes; ++i) dst[i] = src[i];
        d->buf_pos = 0;
        d->buf_end = (int)bytes;
    }

    /* Records are 8-byte-aligned struct dirent images (the server rounds
     * d_reclen to sizeof(off_t) and buf starts 8-aligned), so we can hand
     * back a pointer straight into the buffer. */
    struct dirent *de = (struct dirent *)((char *)d->buf + d->buf_pos);
    if (de->d_reclen == 0) { qsoe_errno = EIO; return 0; }  /* avoid a spin */
    d->buf_pos += de->d_reclen;
    d->tell = de->d_off;
    return de;
}
