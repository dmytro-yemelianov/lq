/*
 * fdseam.c: LQ (seL4) implementation of the fd-operation seam.
 *
 * The OS-independent fcntl()/dup2() (libc/qsoe/) call these; see the seam
 * contract in <sys/qsoe.h>.  seL4 never surfaces the client coid to a
 * server (a server sees only a scoid badge), so LQ cannot use NQ's
 * fresh-connection + _IO_DUP dup (whose resmgr keys on the client coid).
 * Instead LQ duplicates the connection CAP itself (TM_REQ_DUP_CAP): the new
 * fd shares the source's single connection/scoid, so taskman's internal
 * resmgrs need no per-dup work and an external resmgr just refcounts the
 * shared connection (via the _IO_DUP "same scoid" path in libressrv).
 *
 * Per-fd flags (FD_CLOEXEC + the file-status flags) live in a libc-local
 * per-coid word here, not in the kernel connection.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */
#include <fcntl.h>
#include <unistd.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

/* F_GETFL/F_SETFL store the file-status flags in the upper half of the
 * per-coid flag word so they never collide with FD_CLOEXEC (lower half). */
#define FDSEAM_STATUS_SHIFT  16
#define FDSEAM_STATUS_MASK   0xFFFFu

/* Tell taskman to CNode_Copy src_slot into dst_slot (both in the caller's
 * CSpace).  Returns 0 / -1+errno. */
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

/* After a cap-copy dup the new fd shares the old fd's single connection
 * (same cap, same server-side scoid).  An EXTERNAL resmgr must learn of the
 * share so it refcounts the open -- otherwise the first close frees the
 * resmgr's handle out from under the survivor (the bug that broke a shell
 * relocating a script fd off /usr).  Internal (taskman) fds don't need this;
 * taskman keys per-connection.  Send _IO_DUP on newfd, naming oldfd. */
static void dup_notify(int newfd, int oldfd)
{
    struct _server_info si;
    /* ConnectServerInfo returns the matched coid (>=0) on success, -1 on
     * error.  Only an external resmgr (server pid != taskman) keeps a
     * libressrv handle that needs the dup refcount. */
    if (ConnectServerInfo(0, oldfd, &si) < 0 || si.pid == QSOE_PID_TASKMAN)
        return;
    tm_req_io_dup_t dr;
    dr.type = _IO_DUP;
    dr.src_coid = (unsigned long) oldfd;
    dr._reserved[0] = dr._reserved[1] = dr._reserved[2] = 0;
    (void) MsgSend(newfd, &dr, (int) sizeof dr, 0, 0);
}

int qsoe_fd_dupfd(int fd, int start_fd, int cloexec)
{
    unsigned long oldslot = qsoe_state_coid_to_slot(fd);
    if (!oldslot) { qsoe_errno = EBADF; return -1; }

    int newfd = qsoe_state_alloc_coid_ge(start_fd);
    if (newfd < 0) { qsoe_errno = EMFILE; return -1; }

    unsigned long newslot = qsoe_state_alloc_empty_slot();
    if (!newslot) {
        qsoe_state_bind_coid(newfd, 0);     /* roll back the coid reservation */
        qsoe_errno = ENOMEM;
        return -1;
    }
    if (dup_cap_via_taskman(oldslot, newslot) != 0) {
        qsoe_state_free_empty_slot(newslot);
        qsoe_state_bind_coid(newfd, 0);
        return -1;
    }
    qsoe_state_bind_coid(newfd, newslot);
    qsoe_state_set_coid_flags(newfd, cloexec ? FD_CLOEXEC : 0);
    dup_notify(newfd, fd);
    return newfd;
}

int qsoe_fd_cloexec_get(int fd)
{
    if (qsoe_state_coid_to_slot(fd) == 0) { qsoe_errno = EBADF; return -1; }
    return (int) (qsoe_state_get_coid_flags(fd) & FD_CLOEXEC);
}

int qsoe_fd_cloexec_set(int fd, int arg)
{
    if (qsoe_state_coid_to_slot(fd) == 0) { qsoe_errno = EBADF; return -1; }
    unsigned cur = qsoe_state_get_coid_flags(fd);
    cur = (cur & ~(unsigned) FD_CLOEXEC) | ((unsigned) arg & FD_CLOEXEC);
    qsoe_state_set_coid_flags(fd, cur);
    return 0;
}

int qsoe_fd_status_get(int fd)
{
    if (qsoe_state_coid_to_slot(fd) == 0) { qsoe_errno = EBADF; return -1; }
    return (int) (qsoe_state_get_coid_flags(fd) >> FDSEAM_STATUS_SHIFT);
}

int qsoe_fd_status_set(int fd, int arg)
{
    if (qsoe_state_coid_to_slot(fd) == 0) { qsoe_errno = EBADF; return -1; }
    unsigned cur = qsoe_state_get_coid_flags(fd);
    cur = (cur & FDSEAM_STATUS_MASK)
        | (((unsigned) arg & FDSEAM_STATUS_MASK) << FDSEAM_STATUS_SHIFT);
    qsoe_state_set_coid_flags(fd, cur);
    return 0;
}
