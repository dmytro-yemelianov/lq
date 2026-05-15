/*
 * io.c — libqsoe pathmgr-mutation wire wrappers.
 *
 *   qsoe_pathmgr_register / qsoe_pathmgr_repath — used by resmgrs and
 *   init to announce themselves / redirect existing mount points.
 *
 * The POSIX IO surface (open / close / read / write / writev) now
 * lives in userland/libc/qsoe/ and is compiled into libc.a directly
 * — no __sysinfo dispatcher hop.  This file is left for libqsoe
 * primitives that don't have POSIX names.
 */

#include <qsoe-system.h>
#include "../include/qsoe/slots.h"
#include "../include/qsoe/wire.h"
#include "state.h"

#include "sel4_types.h"
#include "qsoe_invoke.h"

static unsigned io_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

/* v0.6.1 pathmgr mutation wire wrappers. Resmgrs use these to
 * announce themselves at boot; init uses repath to redirect existing
 * mounts (e.g. /dev/console -> a real UART driver). */
int qsoe_pathmgr_register(const char *path, int chid)
{
    if (!path) { qsoe_errno = EINVAL; return -1; }
    unsigned plen = io_strlen(path);
    if (plen == 0 || plen >= 128) { qsoe_errno = EINVAL; return -1; }

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < plen; ++i) dst[i] = (unsigned char)path[i];

    seL4_Word mr0 = plen, mr1 = (seL4_Word)chid, mr2 = 0, mr3 = 0;
    unsigned nwords = 4 + (plen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PATHMGR_REGISTER,
                                                   0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
}

int qsoe_pathmgr_repath(const char *path, pid_t new_pid,
                        int new_chid, unsigned handler_kind)
{
    if (!path) { qsoe_errno = EINVAL; return -1; }
    unsigned plen = io_strlen(path);
    if (plen == 0 || plen >= 128) { qsoe_errno = EINVAL; return -1; }

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < plen; ++i) dst[i] = (unsigned char)path[i];

    seL4_Word mr0 = plen, mr1 = (seL4_Word)new_pid,
              mr2 = (seL4_Word)new_chid, mr3 = (seL4_Word)handler_kind;
    unsigned nwords = 4 + (plen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PATHMGR_REPATH,
                                                   0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
}

int qsoe_pathmgr_resolve(const char *path, pid_t *out_pid,
                         int *out_chid, unsigned *out_kind)
{
    if (!path) { qsoe_errno = EINVAL; return -1; }
    unsigned plen = io_strlen(path);
    if (plen == 0 || plen >= 128) { qsoe_errno = EINVAL; return -1; }

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < plen; ++i) dst[i] = (unsigned char)path[i];

    seL4_Word mr0 = plen, mr1 = 0, mr2 = 0, mr3 = 0;
    unsigned nwords = 4 + (plen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_PATHMGR_RESOLVE,
                                                   0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    if (out_pid)  *out_pid  = (pid_t)mr0;
    if (out_chid) *out_chid = (int)mr1;
    if (out_kind) *out_kind = (unsigned)mr2;
    return 0;
}
