/*
 * setxid.c — musl-internal __setxid() backing setuid / setgid /
 *            seteuid / setegid / setreuid / setregid / setresuid /
 *            setresgid.
 *
 * Musl's per-function setuid.c / setgid.c / etc. call
 *   __setxid(nr, a, b, c)
 * where `nr` is the Linux syscall number identifying which exact
 * flavour is being requested.  We translate that into a single
 * TM_REQ_SET_CRED call whose 3 MRs pack the 6 (r/e/s)uid/gid
 * fields, with 0xFFFFFFFF meaning "leave alone".
 *
 * Linux setresXXX semantics:
 *   - argument == -1 (i.e., (unsigned)-1) means "don't change this field"
 *   - other values overwrite
 * setXXid / setXuid map to either setresXXX with two -1's or to a
 * change-both-real-and-effective convention; we follow Linux's
 * setuid()/setgid() (change ruid AND euid if privileged, else just
 * euid) — v0.7 has no privilege check so we apply both.
 */

#include <unistd.h>
#include <sys/syscall.h>
#include <qsoe-system.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

#define KEEP  0xFFFFFFFFu

static int send_set_cred(unsigned ruid, unsigned euid, unsigned suid,
                         unsigned rgid, unsigned egid, unsigned sgid)
{
    seL4_Word mr0 = ((seL4_Word)ruid)        | ((seL4_Word)euid << 32);
    seL4_Word mr1 = ((seL4_Word)suid)        | ((seL4_Word)rgid << 32);
    seL4_Word mr2 = ((seL4_Word)egid)        | ((seL4_Word)sgid << 32);
    seL4_Word mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_SET_CRED, 0, 0, 3);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
}

int __setxid(int nr, int a, int b, int c);
int __setxid(int nr, int a, int b, int c)
{
    /* Cast each argument: -1 from setuid()/etc. arrives as int but
     * we mean "don't change", which our wire op encodes as
     * 0xFFFFFFFF.  Cast through (unsigned int) preserves the bit
     * pattern; we then promote to unsigned for the packed MR. */
    unsigned A = (a < 0) ? KEEP : (unsigned)a;
    unsigned B = (b < 0) ? KEEP : (unsigned)b;
    unsigned C = (c < 0) ? KEEP : (unsigned)c;

    switch (nr) {
    case SYS_setuid:
        /* POSIX setuid(): change ruid AND euid AND suid (privileged) */
        return send_set_cred(A, A, A, KEEP, KEEP, KEEP);
    case SYS_setgid:
        return send_set_cred(KEEP, KEEP, KEEP, A, A, A);
    case SYS_setreuid:
        return send_set_cred(A, B, KEEP, KEEP, KEEP, KEEP);
    case SYS_setregid:
        return send_set_cred(KEEP, KEEP, KEEP, A, B, KEEP);
    case SYS_setresuid:
        return send_set_cred(A, B, C, KEEP, KEEP, KEEP);
    case SYS_setresgid:
        return send_set_cred(KEEP, KEEP, KEEP, A, B, C);
    default:
        qsoe_errno = ENOSYS;
        return -1;
    }
}
