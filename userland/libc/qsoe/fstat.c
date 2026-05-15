/*
 * fstat.c — POSIX fstat().
 *
 * Routes through the connection bound to `fd` so taskman sees the
 * scoid badge of that connection and dispatches to the resmgr's
 * stat handler (cpiofs / console / future external).  The handler
 * fills a tm_stat_t in msg[4..] whose byte layout matches musl's
 * struct stat on RISC-V64 — we copy the bytes into *out_stat.
 */

#include <sys/stat.h>
#include <qsoe/qrv.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

int fstat(int fd, struct stat *out_stat)
{
    if (!out_stat) { qsoe_errno = EINVAL; return -1; }
    seL4_CPtr slot = (seL4_CPtr)qsoe_state_coid_to_slot(fd);
    if (!slot) { qsoe_errno = EBADF; return -1; }

    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_FSTAT, 0, 0, 0);
    seL4_MessageInfo_t reply = qsoe_sys_call(slot, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    unsigned bytes = (unsigned)mr0;
    if (bytes != sizeof *out_stat) { qsoe_errno = EIO; return -1; }

    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    unsigned char *dst = (unsigned char *)out_stat;
    for (unsigned i = 0; i < bytes; ++i) dst[i] = src[i];
    return 0;
}
