/*
 * getcwd.c — POSIX getcwd().
 *
 * Asks taskman (TM_REQ_GETCWD) for the calling process's stored cwd;
 * taskman writes the bytes into msg[4..] and returns the length in
 * MR0.  We copy that into the user buffer.  buf=NULL is the GNU
 * extension that mallocs — not supported in v0.7 (POSIX-strict).
 */

#include <unistd.h>
#include <stddef.h>
#include <qsoe/qrv.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

char *getcwd(char *buf, size_t size)
{
    if (!buf || size == 0) { qsoe_errno = EINVAL; return 0; }

    seL4_Word mr0 = 0, mr1 = 0, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_GETCWD, 0, 0, 0);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return 0; }

    unsigned len = (unsigned)mr0;
    /* POSIX: ERANGE when buf is too small to hold the cwd + NUL. */
    if (len + 1 > size) { qsoe_errno = ERANGE; return 0; }

    const unsigned char *src = (const unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < len; ++i) buf[i] = (char)src[i];
    buf[len] = 0;
    return buf;
}
