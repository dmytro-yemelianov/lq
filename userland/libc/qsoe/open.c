/*
 * open.c — POSIX open() for QSOE.
 *
 * Lives in userland/libc/qsoe/ (surfaced inside the musl source tree
 * as src/os_dependent/open.c via a symlink the libc Makefile creates).
 * Compiled into libc.a alongside the rest of musl, so callers see a
 * plain POSIX open() resolved directly to this function — no
 * __syscall / __sysinfo / dispatcher hop.
 *
 * The body is the v0.5/0.6 qsoe_open() logic moved here verbatim:
 * pack the path into the IPC buffer's msg[4..], TM_REQ_OPEN to
 * taskman, bind the returned cap into the fd/coid namespace.
 */

#include <qsoe/qrv.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

static unsigned path_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

int open(const char *path, int flags, ...);
int open(const char *path, int flags, ...)
{
    (void)flags;  /* mode (variadic) and flags ignored for now */

    if (!path) { qsoe_errno = EINVAL; return -1; }
    unsigned plen = path_strlen(path);
    if (plen == 0 || plen >= 128) { qsoe_errno = EINVAL; return -1; }

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < plen; ++i) dst[i] = (unsigned char)path[i];

    seL4_Word mr0 = plen, mr1 = 0, mr2 = 0, mr3 = 0;
    unsigned nwords = 4 + (plen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_OPEN, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    seL4_CPtr cap_slot = (seL4_CPtr)mr0;
    int fd = qsoe_state_alloc_coid(0);
    if (fd < 0) { qsoe_errno = ENOMEM; return -1; }
    qsoe_state_bind_coid(fd, cap_slot);
    return fd;
}
