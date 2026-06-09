/*
 * readlink.c — POSIX readlink().
 *
 * Sends TM_REQ_READLINK to taskman with the path in msg[4..].
 * taskman resolves through pathmgr and asks the resmgr to fill the
 * link target; for QSOE v0.7 no symlinks exist so taskman replies
 * with EINVAL for any path that resolves, ENOENT otherwise.  When
 * symlinks land, the target bytes ride back in msg[4..] sized via
 * MR0 — this code already copies them out.
 */

#include <unistd.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

static unsigned path_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

ssize_t readlink(const char *path, char *buf, size_t bufsize)
{
    if (!path || !buf || bufsize == 0) { qsoe_errno = EINVAL; return -1; }
    unsigned plen = path_strlen(path);
    if (plen == 0 || plen >= 128) { qsoe_errno = ENAMETOOLONG; return -1; }

    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < plen; ++i) dst[i] = (unsigned char)path[i];

    seL4_Word mr0 = plen, mr1 = 0, mr2 = 0, mr3 = 0;
    unsigned nwords = 4 + (plen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_READLINK, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    unsigned bytes = (unsigned)mr0;
    if (bytes > bufsize) bytes = (unsigned)bufsize;
    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < bytes; ++i) buf[i] = (char)src[i];
    /* POSIX: readlink does NOT NUL-terminate.  Caller's responsibility. */
    return (ssize_t)bytes;
}
