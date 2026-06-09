/*
 * fcntl.c — POSIX fcntl().
 *
 * Implemented subcommands (real, not stubs):
 *   F_DUPFD          — first free fd ≥ arg, cap copied via TM_REQ_DUP_CAP
 *   F_DUPFD_CLOEXEC  — F_DUPFD + sets FD_CLOEXEC on the new fd
 *   F_GETFD / F_SETFD — close-on-exec bit, stored per-fd in libqsoe
 *   F_GETFL / F_SETFL — file-status flags (O_NONBLOCK …), per-fd
 *
 * Unimplemented: F_GETLK / F_SETLK / F_SETLKW (POSIX advisory locks)
 * return -1 with ENOSYS — QSOE has no fcntl locking yet.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

/* Tell taskman to CNode_Copy oldfd_slot into newfd_slot.  Shared
 * mechanism with dup2.c; inlined here to avoid an internal header
 * just for one helper. */
static int dup_cap_via_taskman(unsigned long src_slot, unsigned long dst_slot)
{
    seL4_Word mr0 = src_slot, mr1 = dst_slot, mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_DUP_CAP, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }
    return 0;
}

static int fcntl_dupfd(int fd, int start_fd, int set_cloexec)
{
    unsigned long oldslot = qsoe_state_coid_to_slot(fd);
    if (!oldslot) { qsoe_errno = EBADF; return -1; }
    int newfd = qsoe_state_alloc_coid_ge(start_fd);
    if (newfd < 0) { qsoe_errno = EMFILE; return -1; }

    unsigned long newslot = qsoe_state_alloc_empty_slot();
    if (!newslot) {
        /* Roll back the coid reservation. */
        qsoe_state_bind_coid(newfd, 0);
        qsoe_errno = ENOMEM;
        return -1;
    }
    if (dup_cap_via_taskman(oldslot, newslot) != 0) {
        qsoe_state_free_empty_slot(newslot);
        qsoe_state_bind_coid(newfd, 0);
        return -1;
    }
    qsoe_state_bind_coid(newfd, newslot);
    qsoe_state_set_coid_flags(newfd, set_cloexec ? FD_CLOEXEC : 0);
    return newfd;
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);

    switch (cmd) {
    case F_DUPFD: {
        int start_fd = va_arg(ap, int);
        va_end(ap);
        return fcntl_dupfd(fd, start_fd, /*cloexec=*/0);
    }
    case F_DUPFD_CLOEXEC: {
        int start_fd = va_arg(ap, int);
        va_end(ap);
        return fcntl_dupfd(fd, start_fd, /*cloexec=*/1);
    }
    case F_GETFD: {
        va_end(ap);
        if (qsoe_state_coid_to_slot(fd) == 0) { qsoe_errno = EBADF; return -1; }
        return (int)(qsoe_state_get_coid_flags(fd) & FD_CLOEXEC);
    }
    case F_SETFD: {
        int arg = va_arg(ap, int);
        va_end(ap);
        if (qsoe_state_coid_to_slot(fd) == 0) { qsoe_errno = EBADF; return -1; }
        unsigned cur = qsoe_state_get_coid_flags(fd);
        cur = (cur & ~(unsigned)FD_CLOEXEC) | ((unsigned)arg & FD_CLOEXEC);
        qsoe_state_set_coid_flags(fd, cur);
        return 0;
    }
    case F_GETFL: {
        va_end(ap);
        if (qsoe_state_coid_to_slot(fd) == 0) { qsoe_errno = EBADF; return -1; }
        /* F_GETFL returns the file-status flags (O_NONBLOCK, O_APPEND,
         * access mode).  We store them in the upper bits of the
         * fd-flag word so they don't collide with FD_CLOEXEC. */
        return (int)(qsoe_state_get_coid_flags(fd) >> 16);
    }
    case F_SETFL: {
        int arg = va_arg(ap, int);
        va_end(ap);
        if (qsoe_state_coid_to_slot(fd) == 0) { qsoe_errno = EBADF; return -1; }
        unsigned cur = qsoe_state_get_coid_flags(fd);
        cur = (cur & 0xFFFFu) | (((unsigned)arg & 0xFFFFu) << 16);
        qsoe_state_set_coid_flags(fd, cur);
        return 0;
    }
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        va_end(ap);
        qsoe_errno = ENOSYS;
        return -1;
    default:
        va_end(ap);
        qsoe_errno = EINVAL;
        return -1;
    }
}
