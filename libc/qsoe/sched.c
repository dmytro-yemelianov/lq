/*
 * lq/libc/qsoe/sched.c -- LQ seam: SchedSet / SchedGet.
 *
 * QNX-shape thread scheduling control, routed to taskman over seL4 IPC.
 * SchedSet adjusts a thread's seL4 priority (via TCB_SetPriority in
 * taskman) and records its policy; SchedGet reads both back -- taskman
 * tracks them because seL4 has no get-priority invocation.
 *
 * pid == 0 means "the calling process" (taskman maps it via the request
 * badge).  tid == 0 means "the calling thread": only the client knows
 * which thread it is, so we resolve it here from qsoe_curthr() before
 * sending.  tid == 1 is the main thread; workers are >= 2.
 *
 * SchedYield lives next door in timer.c; SchedInfo / SchedCtl are not
 * wired yet.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qsoe.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sched.h>
#include <sys/qsoe.h>
#include <qsoe/slots.h>     /* QSOE_CAP_TASKMAN_EP */
#include <qsoe/tm_msgs.h>

#include "sel4_types.h"
#include "qsoe_invoke.h"

/* tid == 0 -> the calling thread's own tid. */
static int resolve_tid(int tid)
{
    return tid != 0 ? tid : qsoe_curthr()->tid;
}

long SchedSet_r(pid_t pid, int tid, int policy, const struct sched_param *param)
{
    if (!param) return -EINVAL;
    seL4_Word mr0 = (seL4_Word)pid;
    seL4_Word mr1 = (seL4_Word)resolve_tid(tid);
    seL4_Word mr2 = (seL4_Word)policy;
    seL4_Word mr3 = (seL4_Word)param->sched_priority;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_SCHED_SET, 0, 0, 4);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                             &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    return err ? -(long)err : 0;
}

int SchedSet(pid_t pid, int tid, int policy, const struct sched_param *param)
{
    long r = SchedSet_r(pid, tid, policy, param);
    if (r < 0) { qsoe_errno = (int)(-r); return -1; }
    return 0;
}

/* Returns the scheduling policy on success (>= 0), or a negative errno. */
long SchedGet_r(pid_t pid, int tid, struct sched_param *param)
{
    seL4_Word mr0 = (seL4_Word)pid;
    seL4_Word mr1 = (seL4_Word)resolve_tid(tid);
    seL4_Word mr2 = 0, mr3 = 0;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_SCHED_GET, 0, 0, 2);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                             &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err) return -(long)err;
    if (param) param->sched_priority = (int)mr0;   /* reply mr0 = priority */
    return (long)(int)mr1;                          /* reply mr1 = policy   */
}

int SchedGet(pid_t pid, int tid, struct sched_param *param)
{
    long r = SchedGet_r(pid, tid, param);
    if (r < 0) { qsoe_errno = (int)(-r); return -1; }
    return (int)r;
}
