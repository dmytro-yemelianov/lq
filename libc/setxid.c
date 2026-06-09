/*
 * setxid.c — musl-internal __setxid() for LQ — backs setuid / setgid /
 *            seteuid / setegid / setreuid / setregid / setresuid /
 *            setresgid.
 *
 * The shared OS-independent libc body keeps the per-function wrappers
 * (setuid.c, setgid.c, ...) that call __setxid(nr, a, b, c), where `nr`
 * is a flavour-dispatch tag — internal to this TU and NEVER reaches
 * the kernel.  We switch on it and emit a TM_REQ_SET_CRED to taskman
 * over seL4 IPC; the message packs all six (r/e/s)uid/gid fields,
 * with KEEP meaning "leave this one alone".
 *
 * setresXXX semantics: an argument of -1 means "don't change"; v0.x
 * has no privilege check, so setuid()/setgid() change all three of
 * r/e/s.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include <sel4_types.h>
#include <qsoe_invoke.h>

/* Local flavour tags — internal dispatch keys, NOT Linux syscall
 * numbers.  Upstream musl reused Linux SYS_set{uid,gid,reuid,regid,
 * resuid,resgid} here, which we explicitly retired.  Values are
 * arbitrary; they never leave this TU. */
#define SYS_setuid     1
#define SYS_setgid     2
#define SYS_setreuid   3
#define SYS_setregid   4
#define SYS_setresuid  5
#define SYS_setresgid  6

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
