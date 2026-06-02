/*
 * access.c — POSIX access().
 *
 * Sends TM_REQ_ACCESS to taskman with the path in msg[4..].  v0.7
 * collapses mode bits (F_OK / R_OK / W_OK / X_OK) into a single
 * existence check because every QSOE process runs as root and every
 * resmgr-owned path is fully accessible.  The mode argument is
 * accepted for ABI / source compatibility.
 *
 * When multi-user / per-file permissions land, this remains a thin
 * wrapper — the mode bits travel in MR1 (currently zero) and
 * taskman starts enforcing.
 */

#include <unistd.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

static unsigned path_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

int access(const char *path, int mode)
{
    (void)mode;
    if (!path) { qsoe_errno = EFAULT; return -1; }
    unsigned plen = path_strlen(path);
    if (plen == 0 || plen >= 128) { qsoe_errno = ENAMETOOLONG; return -1; }

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < plen; ++i) dst[i] = (unsigned char)path[i];

    seL4_Word mr0 = plen, mr1 = 0, mr2 = 0, mr3 = 0;
    unsigned nwords = 4 + (plen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_ACCESS, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
}
