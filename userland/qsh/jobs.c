/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/* cross fingers and hope kill is killpg-endowed */
#define qsh_killpg(p, s) kill(-(p), (s))

/* Order important! */
#define PRUNNING 0
#define PEXITED 1
#define PSIGNALLED 2
#define PSTOPPED 3

#define PROC_TGTSZ 256U

typedef struct proc Proc;
/* to take alignment into consideration */
struct proc_dummy {
    Proc *next;
    int state;
    int status;
    pid_t pid;
    char command[PROC_TGTSZ - (ALLOC_OVERHEAD + 14U)];
};
#define PROC_OFS (offsetof(struct proc_dummy, command))
/* real structure */
struct proc {
    /* next process in pipeline (if any) */
    Proc *next;
    /* one of the four P… above */
    int state;
    /* wait status */
    int status;
    /* process ID of this Unix process in the job */
    pid_t pid;
    /* process command string from vistree */
    char command[PROC_TGTSZ - (ALLOC_OVERHEAD + PROC_OFS)];
};

/* Notify/print flag - j_print() argument */
#define JP_SHORT 1  /* print signals processes were killed by */
#define JP_MEDIUM 2 /* print [job-num] -/+ command */
#define JP_LONG 3   /* print [job-num] -/+ pid command */
#define JP_PGRP 4   /* print pgrp */

/* put_job() flags */
#define PJ_ON_FRONT 0     /* at very front */
#define PJ_PAST_STOPPED 1 /* just past any stopped jobs */

/* Job.flags values */
#define JF_STARTED 0x001       /* set when all processes in job are started */
#define JF_WAITING 0x002       /* set if j_waitj() is waiting on job */
#define JF_W_ASYNCNOTIFY 0x004 /* set if waiting and async notification ok */
#define JF_XXCOM 0x008         /* set for $(command) jobs */
#define JF_FG 0x010            /* running in foreground (also has tty pgrp) */
#define JF_SAVEDTTY 0x020      /* j->ttystat is valid */
#define JF_CHANGED 0x040       /* process has changed state */
#define JF_KNOWN 0x080         /* $! referenced */
#define JF_ZOMBIE 0x100        /* known, unwaited process */
#define JF_REMOVE 0x200        /* flagged for removal (j_jobs()/j_noityf()) */
/* JF_USETTYMODE retired with the termios scaffolding (v0.6.3). */
#define JF_SAVEDTTYPGRP 0x800  /* j->saved_ttypgrp is valid */

typedef struct job Job;
struct job {
    ALLOC_ITEM alloc_INT;   /* internal, do not touch */
    Job *next;              /* next job in list */
    Proc *proc_list;        /* process list */
    Proc *last_proc;        /* last process in list */
    struct timeval systime; /* system time used by job */
    struct timeval usrtime; /* user time used by job */
    pid_t pgrp;             /* process group of job */
    pid_t ppid;             /* pid of process that forked job */
    int job;                /* job number: %n */
    int flags;              /* see JF_* */
    volatile int state;     /* job state */
    int status;             /* exit status of last process */
    int age;                /* number of jobs started */
    Coproc_id coproc_id;    /* 0 or id of coprocess output pipe */
};

/* Flags for j_waitj() */
#define JW_NONE 0x00
#define JW_INTERRUPT 0x01   /* ^C will stop the wait */
#define JW_ASYNCNOTIFY 0x02 /* asynchronous notification during wait ok */
#define JW_STOPPEDWAIT 0x04 /* wait even if job stopped */
#define JW_PIPEST 0x08      /* want PIPESTATUS */

/* Error codes for j_lookup() */
#define JL_NOSUCH 0  /* no such job */
#define JL_AMBIG 1   /* %foo or %?foo is ambiguous */
#define JL_INVALID 2 /* non-pid, non-% job id */

const char Tpipest[] = "PIPESTATUS";

static const char *const lookup_msgs[] = {"no such job", "ambiguous",
                                          "argument must be %job or process id"};

static Job *job_list; /* job list */
static Job *last_job;
static Job *async_job;
static pid_t async_pid;

static int nzombie; /* # of zombies owned by this process */
static int njobs;   /* # of jobs started */

#ifndef CHILD_MAX
#define CHILD_MAX 25
#endif

/* held_sigchld is set if sigchld occurs before a job is completely started */
static volatile sig_atomic_t held_sigchld;

static void j_startjob(Job *);
static int j_waitj(Job *, int, const char *);
static void j_sigchld(int);
static void j_print(Job *, int, struct shf *);
static Job *j_lookup(const char *, int *);
static Job *new_job(void);
static Proc *new_proc(void);
static void check_job(Job *);
static void put_job(Job *, int);
static void remove_job(Job *, const char *);
static int kill_job(Job *, int);

static void tty_init_talking(void);

static void vistree(char *, size_t, struct op *) QSH_A_BOUNDED(__string__, 1, 2);

/* initialise job control */
void
j_init(void)
{
    sigemptyset(&sm_default);
    sigprocmask(SIG_SETMASK, &sm_default, NULL);

    sigemptyset(&sm_sigchld);
    sigaddset(&sm_sigchld, SIGCHLD);

    setsig(&sigtraps[SIGCHLD], j_sigchld, SS_RESTORE_ORIG | SS_FORCE | SS_SHTRAP);

    if (Flag(FTALKING)) {
        tty_init_talking();
    }
}

static int
proc_errorlevel(Proc *p)
{
    switch (p->state) {
    case PEXITED:
        return ((WEXITSTATUS(p->status)) & 255);
    case PSIGNALLED:
        /* coverity[result_independent_of_operands : SUPPRESS] */
        return (qsh_sigmask(WTERMSIG(p->status)));
    default:
        return (0);
    }
}

/* job cleanup before shell exit */
void
j_exit(void)
{
    /* kill stopped, and possibly running, jobs */
    Job *j;
    bool killed = false;

    for (j = job_list; j != NULL; j = j->next) {
        if (j->ppid == procpid &&
            (j->state == PSTOPPED ||
             (j->state == PRUNNING &&
              ((j->flags & JF_FG) || (Flag(FLOGIN) && !Flag(FNOHUP) && procpid == qshpid))))) {
            killed = true;
            if (j->pgrp == 0)
                kill_job(j, SIGHUP);
            else
                qsh_killpg(j->pgrp, SIGHUP);
        }
    }
    if (killed)
        sleep(1);
    j_notify();

}

/* execute tree in child subprocess */
int
exchild(struct op *t, int flags, volatile int *xerrok,
        /* used if XPCLOSE or XCCLOSE */
        int close_fd)
{
    /* for pipelines */
    static Proc *last_proc;

    int rv = 0, forksleep, jwflags = JW_NONE;
    int eno = /* stupid GCC */ 0;
    sigset_t omask;
    Proc *p;
    Job *j;
    pid_t cldpid;

    if (flags & XPIPEST) {
        flags &= ~XPIPEST;
        jwflags |= JW_PIPEST;
    }

    if (flags & XEXEC)
        /*
         * Clear XFORK|XPCLOSE|XCCLOSE|XCOPROC|XPIPEO|XPIPEI|XXCOM|XBGND
         * (also done in another execute() below)
         */
        return (execute(t, flags & (XEXEC | XERROK), xerrok));

    /* no SIGCHLDs while messing with job and process lists */
    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);

    p = new_proc();
    p->next = NULL;
    p->state = PRUNNING;
    p->status = 0;
    p->pid = 0;

    /* link process into jobs list */
    if (flags & XPIPEI) {
        /* continuing with a pipe */
        if (!last_job)
            kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO,
                   "exchild: XPIPEI and no last_job - pid %ld", (long)procpid);
        j = last_job;
        if (last_proc)
            last_proc->next = p;
        last_proc = p;
    } else {
        /* fills in j->job */
        j = new_job();
        /*
         * we don't consider XXCOMs foreground since they don't get
         * tty process group and we don't save or restore tty modes.
         */
        j->flags = (flags & XXCOM) ? JF_XXCOM : ((flags & XBGND) ? 0 : JF_FG);
        timerclear(&j->usrtime);
        timerclear(&j->systime);
        j->state = PRUNNING;
        j->pgrp = 0;
        j->ppid = procpid;
        j->age = ++njobs;
        j->proc_list = p;
        j->coproc_id = 0;
        last_job = j;
        last_proc = p;
        put_job(j, PJ_PAST_STOPPED);
    }

    vistree(p->command, sizeof(p->command), t);

    /*
     * QRV: no fork().  Run the AST in this process — the
     * "stay-resident subshell" model.  External commands inside
     * the AST go through posix_spawn() at exec.c's TEXEC site,
     * so the shell remains alive throughout.  This drops most of
     * the upstream child-side bookkeeping: signal restoration,
     * /dev/null redirect for backgrounded jobs, env teardown,
     * job-list pruning of the executing job — none of those
     * apply when there is no separate child to set up.
     *
     * Background and pipeline support is therefore degenerate
     * for now (single-threaded, blocking).  Adding async via
     * posix_spawn detached jobs is a follow-up — the structure
     * here is the place to do it.
     */
    /*
     * XEXEC must NOT be set here: it means "I'm a forked child,
     * unwind(LEXIT) when done."  We have no child — passing it
     * exits the shell after one command.  The `exec command`
     * builtin path keeps XEXEC and short-circuits at line 411
     * above, so its own LEXIT semantics still work.
     */
    p->pid = procpid;
    rv = execute(t, flags & XERROK, xerrok);
    p->state = PEXITED;
    p->status = (rv & 0xFF) << 8;

    if (close_fd >= 0 && (flags & XPCLOSE))
        close(close_fd);

    /* parent-side bookkeeping that still makes sense */
    if (!(flags & XPIPEO)) {
        j_startjob(j);
        if (flags & XCOPROC) {
            j->coproc_id = coproc.id;
            coproc.njobs++;
            coproc.job = (void *)j;
        }
        /* in this single-threaded model the job is already done */
        j->state = PEXITED;
        remove_job(j, "exchild done");
    }

    sigprocmask(SIG_SETMASK, &omask, NULL);

    (void)cldpid; (void)forksleep; (void)eno; (void)jwflags;
    return (rv);
}

/* start the last job: only used for $(command) jobs */
void
startlast(void)
{
    sigset_t omask;

    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);

    /* no need to report error - waitlast() will do it */
    if (last_job) {
        /* ensure it isn't removed by check_job() */
        last_job->flags |= JF_WAITING;
        j_startjob(last_job);
    }
    sigprocmask(SIG_SETMASK, &omask, NULL);
}

/* wait for last job: only used for $(command) jobs */
int
waitlast(void)
{
    int rv;
    Job *j;
    sigset_t omask;

    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);

    j = last_job;
    if (!j || !(j->flags & JF_STARTED)) {
        if (!j)
            kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_TWOMSG | KWF_NOERRNO, "waitlast", "no last job");
        else
            kwarnf(KWF_INTERNAL | KWF_WARNING | KWF_TWOMSG | KWF_NOERRNO, "waitlast", Tnot_started);
        sigprocmask(SIG_SETMASK, &omask, NULL);
        /* not so arbitrary, non-zero value */
        return (125);
    }

    rv = j_waitj(j, JW_NONE, "waitlast");

    sigprocmask(SIG_SETMASK, &omask, NULL);

    return (rv);
}

/* wait for child, interruptable. */
int
waitfor(const char *cp, int *sigp)
{
    int rv, ecode, flags = JW_INTERRUPT | JW_ASYNCNOTIFY;
    Job *j;
    sigset_t omask;

    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);

    *sigp = 0;

    if (cp == NULL) {
        /*
         * wait for an unspecified job - always returns 0, so
         * don't have to worry about exited/signaled jobs
         */
        for (j = job_list; j; j = j->next)
            /* AT&T ksh will wait for stopped jobs - we don't */
            if (j->ppid == procpid && j->state == PRUNNING)
                break;
        if (!j) {
            sigprocmask(SIG_SETMASK, &omask, NULL);
            return (-1);
        }
    } else if ((j = j_lookup(cp, &ecode))) {
        /* don't report normal job completion */
        flags &= ~JW_ASYNCNOTIFY;
        if (j->ppid != procpid) {
            sigprocmask(SIG_SETMASK, &omask, NULL);
            return (-1);
        }
    } else {
        sigprocmask(SIG_SETMASK, &omask, NULL);
        if (ecode != JL_NOSUCH)
            bi_errorf(Tf_sD_s, cp, lookup_msgs[ecode]);
        return (-1);
    }

    /* AT&T ksh will wait for stopped jobs - we don't */
    rv = j_waitj(j, flags, "jw:waitfor");

    sigprocmask(SIG_SETMASK, &omask, NULL);

    if (rv < 0)
        /* we were interrupted */
        *sigp = qsh_sigmask(-rv);

    return (rv);
}

/* kill (built-in) a job */
int
j_kill(const char *cp, int sig)
{
    Job *j;
    int rv = 0, ecode;
    sigset_t omask;

    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);

    if ((j = j_lookup(cp, &ecode)) == NULL) {
        sigprocmask(SIG_SETMASK, &omask, NULL);
        bi_errorf(Tf_sD_s, cp, lookup_msgs[ecode]);
        return (1);
    }

    if (j->pgrp == 0) {
        /* started when !Flag(FMONITOR) */
        if (kill_job(j, sig) < 0) {
            bi_errorf(Tf_sD_s, cp, cstrerror(errno));
            rv = 1;
        }
    } else {
        if (qsh_killpg(j->pgrp, sig) < 0) {
            bi_errorf(Tf_sD_s, cp, cstrerror(errno));
            rv = 1;
        }
    }

    sigprocmask(SIG_SETMASK, &omask, NULL);

    return (rv);
}

/* are there any running or stopped jobs ? */
int
j_stopped_running(void)
{
    Job *j;
    int which = 0;

    for (j = job_list; j != NULL; j = j->next) {
        if (Flag(FLOGIN) && !Flag(FNOHUP) && procpid == qshpid && j->ppid == procpid &&
            j->state == PRUNNING)
            which |= 2;
    }
    if (which) {
        shellf("You have %s%s%s jobs\n", which & 1 ? "stopped" : "", which == 3 ? " and " : "",
               which & 2 ? "running" : "");
        return (1);
    }

    return (0);
}

/* list jobs for jobs built-in */
int
j_jobs(const char *cp, int slp,
       /* 0: short, 1: long, 2: pgrp */
       int nflag)
{
    Job *j, *tmp;
    int how, zflag = 0;
    sigset_t omask;

    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);

    if (nflag < 0) {
        /* kludge: print zombies */
        nflag = 0;
        zflag = 1;
    }
    if (cp) {
        int ecode;

        if ((j = j_lookup(cp, &ecode)) == NULL) {
            sigprocmask(SIG_SETMASK, &omask, NULL);
            bi_errorf(Tf_sD_s, cp, lookup_msgs[ecode]);
            return (1);
        }
    } else
        j = job_list;
    how = slp == 0 ? JP_MEDIUM : (slp == 1 ? JP_LONG : JP_PGRP);
    for (; j; j = j->next) {
        if ((!(j->flags & JF_ZOMBIE) || zflag) && (!nflag || (j->flags & JF_CHANGED))) {
            j_print(j, how, shl_stdout);
            if (j->state == PEXITED || j->state == PSIGNALLED)
                j->flags |= JF_REMOVE;
        }
        if (cp)
            break;
    }
    /* Remove jobs after printing so there won't be multiple + or - jobs */
    for (j = job_list; j; j = tmp) {
        tmp = j->next;
        if (j->flags & JF_REMOVE)
            remove_job(j, Tjobs);
    }
    sigprocmask(SIG_SETMASK, &omask, NULL);
    return (0);
}

/* list jobs for top-level notification */
void
j_notify(void)
{
    Job *j, *tmp;
    sigset_t omask;

    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);
    for (j = job_list; j; j = j->next) {
        /*
         * Remove job after doing reports so there aren't
         * multiple +/- jobs.
         */
        if (j->state == PEXITED || j->state == PSIGNALLED)
            j->flags |= JF_REMOVE;
    }
    for (j = job_list; j; j = tmp) {
        tmp = j->next;
        if (j->flags & JF_REMOVE) {
            if (j == async_job || (j->flags & JF_KNOWN)) {
                j->flags = (j->flags & ~JF_REMOVE) | JF_ZOMBIE;
                j->job = -1;
                nzombie++;
            } else
                remove_job(j, "notify");
        }
    }
    shf_flush(shl_out);
    sigprocmask(SIG_SETMASK, &omask, NULL);
}

/* Return pid of last process in last asynchronous job */
pid_t
j_async(void)
{
    sigset_t omask;

    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);

    if (async_job)
        async_job->flags |= JF_KNOWN;

    sigprocmask(SIG_SETMASK, &omask, NULL);

    return (async_pid);
}

/*
 * Start a job: set STARTED, check for held signals and set j->last_proc
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static void
j_startjob(Job *j)
{
    Proc *p;

    j->flags |= JF_STARTED;
    for (p = j->proc_list; p->next; p = p->next)
        ;
    j->last_proc = p;

    if (held_sigchld) {
        held_sigchld = 0;
        /* Don't call j_sigchld() as it may remove job... */
        kill(procpid, SIGCHLD);
    }
}

/*
 * wait for job to complete or change state
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static int
j_waitj(Job *j,
        /* see JW_* */
        int flags, const char *where)
{
    Proc *p;
    int rv;
    sigset_t omask;

    /*
     * No auto-notify on the job we are waiting on.
     */
    j->flags |= JF_WAITING;
    if (flags & JW_ASYNCNOTIFY)
        j->flags |= JF_W_ASYNCNOTIFY;

        flags |= JW_STOPPEDWAIT;

    while (j->state == PRUNNING || ((flags & JW_STOPPEDWAIT) && j->state == PSTOPPED)) {
        sigprocmask(SIG_SETMASK, &sm_default, &omask);
        pause();
        /* note that handlers may run here so they need to know */
        sigprocmask(SIG_SETMASK, &omask, NULL);
        if (fatal_trap) {
            int oldf = j->flags & (JF_WAITING | JF_W_ASYNCNOTIFY);
            j->flags &= ~(JF_WAITING | JF_W_ASYNCNOTIFY);
            runtraps(TF_FATAL);
            /* not reached... */
            j->flags |= oldf;
        }
        if ((flags & JW_INTERRUPT) && (rv = trap_pending())) {
            j->flags &= ~(JF_WAITING | JF_W_ASYNCNOTIFY);
            return (-rv);
        }
    }
    j->flags &= ~(JF_WAITING | JF_W_ASYNCNOTIFY);

    if (j->flags & JF_FG) {
        j->flags &= ~JF_FG;
        /* tty save/restore around fg jobs is a no-op on QSOE: there
         * is no canonical-mode tty driver to negotiate with. */
    }

    j_usrtime = j->usrtime;
    j_systime = j->systime;
    rv = j->status;

    if (!(p = j->proc_list)) {
        ; /* nothing */
    } else if (flags & JW_PIPEST) {
        k32 num = 0;
        struct tbl *vp;
        kby *vt;

        unset(vp_pipest, 1);
        vp = vp_pipest;
        vp->flag = DEFINED | ISSET | INTEGER | RDONLY | ARRAY | INT_U;
        goto got_array;

        while (p != NULL) {
            vt = alloc(qccFAMSZ(struct tbl, name, sizeof(Tpipest)), vp_pipest->areap);
            memset(vt, 0, offsetof(struct tbl, name));
            memcpy(vt + offsetof(struct tbl, name), Tpipest, sizeof(Tpipest));
            vp->u.array = (void *)vt;
            vp = (void *)vt;
            vp->areap = vp_pipest->areap;
            vp->ua.index = num = qiMO(k32, K32_FM, num, +, 1U);
            vp->flag = DEFINED | ISSET | INTEGER | RDONLY | ARRAY | INT_U | AINDEX;
        got_array:
            vp->val.i = proc_errorlevel(p);
            if (Flag(FPIPEFAIL) && vp->val.i)
                rv = vp->val.i;
            p = p->next;
        }
    } else if (Flag(FPIPEFAIL)) {
        do {
            const int i = proc_errorlevel(p);

            if (i)
                rv = i;
        } while ((p = p->next));
    }

    if (!(flags & JW_ASYNCNOTIFY)
    ) {
        j_print(j, JP_SHORT, shl_out);
        shf_flush(shl_out);
    }
    if (j->state != PSTOPPED
    )
        remove_job(j, where);

    return (rv);
}

/*
 * SIGCHLD handler to reap children and update job states
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
/* ARGSUSED */
static void
j_sigchld(int sig QSH_A_UNUSED)
{
    int saved_errno = errno;
    Job *j;
    Proc *p = NULL;
    pid_t pid;
    int status;
    struct rusage ru0, ru1;
    sigset_t omask;

    /* this handler can run while SIGCHLD is not blocked, so block it now */
    sigprocmask(SIG_BLOCK, &sm_sigchld, &omask);

    /*
     * Don't wait for any processes if a job is partially started.
     * This is so we don't do away with the process group leader
     * before all the processes in a pipe line are started (so the
     * setpgid() won't fail)
     */
    for (j = job_list; j; j = j->next)
        if (j->ppid == procpid && !(j->flags & JF_STARTED)) {
            held_sigchld = 1;
            goto j_sigchld_out;
        }

    if (qsh_getrusage(RUSAGE_CHILDREN, &ru0))
        kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG, Tgetrusage);
    do {
        pid = waitpid(-1, &status,
                      (WNOHANG |
#if defined(WCONTINUED) && defined(WIFCONTINUED)
                       WCONTINUED |
#endif
                       WUNTRACED));

        /*
         * return if this would block (0) or no children
         * or interrupted (-1)
         */
        if (pid <= 0)
            goto j_sigchld_out;

        if (qsh_getrusage(RUSAGE_CHILDREN, &ru1))
            kwarnf(KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG, Tgetrusage);

        /* find job and process structures for this pid */
        for (j = job_list; j != NULL; j = j->next)
            for (p = j->proc_list; p != NULL; p = p->next)
                if (p->pid == pid)
                    goto found;
    found:
        if (j == NULL) {
            /* Can occur if process has kids, then execs shell
            kwarnf0(KWF_PREFIX | KWF_FILELINE | KWF_NOERRNO,
                "bad process waited for (pid = %d)", pid);
             */
            ru0 = ru1;
            continue;
        }

        timeradd(&j->usrtime, &ru1.ru_utime, &j->usrtime);
        timersub(&j->usrtime, &ru0.ru_utime, &j->usrtime);
        timeradd(&j->systime, &ru1.ru_stime, &j->systime);
        timersub(&j->systime, &ru0.ru_stime, &j->systime);
        ru0 = ru1;
        p->status = status;
            if (WIFSIGNALED(status))
            p->state = PSIGNALLED;
        else
            p->state = PEXITED;

        /* check to see if entire job is done */
        check_job(j);
    }
    while (/* CONSTCOND */ 1);

j_sigchld_out:
    sigprocmask(SIG_SETMASK, &omask, NULL);
    errno = saved_errno;
}

/*
 * Called only when a process in j has exited/stopped (ie, called only
 * from j_sigchld()). If no processes are running, the job status
 * and state are updated, asynchronous job notification is done and,
 * if unneeded, the job is removed.
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static void
check_job(Job *j)
{
    int jstate;
    Proc *p;

    /* XXX debugging (nasty - interrupt routine using shl_out) */
    if (!(j->flags & JF_STARTED)) {
        kwarnf0(KWF_INTERNAL | KWF_WARNING | KWF_NOERRNO, "check_job: job started (flags 0x%X)",
                (unsigned int)j->flags);
        return;
    }

    jstate = PRUNNING;
    for (p = j->proc_list; p != NULL; p = p->next) {
        if (p->state == PRUNNING)
            /* some processes still running */
            return;
        if (p->state > jstate)
            jstate = p->state;
    }
    j->state = jstate;
    j->status = proc_errorlevel(j->last_proc);

    /*
     * Note when co-process dies: can't be done in j_wait() nor
     * remove_job() since neither may be called for non-interactive
     * shells.
     */
    if (j->state == PEXITED || j->state == PSIGNALLED) {
        /*
         * No need to keep co-process input any more
         * (at least, this is what ksh93d thinks)
         */
        if (coproc.job == j) {
            coproc.job = NULL;
            /*
             * XXX would be nice to get the closes out of here
             * so they aren't done in the signal handler.
             * Would mean a check in coproc_getfd() to
             * do "if job == 0 && write >= 0, close write".
             */
            coproc_write_close(coproc.write);
        }
        /* Do we need to keep the output? */
        if (j->coproc_id && j->coproc_id == coproc.id && --coproc.njobs == 0)
            coproc_readw_close(coproc.read);
    }

    j->flags |= JF_CHANGED;
    if (
        !(j->flags & (JF_WAITING | JF_FG)) && j->state != PSTOPPED) {
        if (j == async_job || (j->flags & JF_KNOWN)) {
            j->flags |= JF_ZOMBIE;
            j->job = -1;
            nzombie++;
        } else
            remove_job(j, "checkjob");
    }
}

/*
 * Print job status in either short, medium or long format.
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static void
j_print(Job *j, int how, struct shf *shf)
{
    Proc *p;
    int state;
    int status;
#ifdef WCOREDUMP
    bool coredumped;
#endif
    char jobchar;
    bool output = false;
    const char *msg;
    const char *filler;
    char msgbuf[sizeof("Done (255)")];

    if (how == JP_PGRP) {
        /*
         * POSIX doesn't say what to do it there is no process
         * group leader (ie, !FMONITOR). We arbitrarily return
         * last pid (which is what $! returns).
         */
        shf_fprintf(shf, Tf_dN, (int)(j->pgrp ? j->pgrp : (j->last_proc ? j->last_proc->pid : 0)));
        return;
    }
    /* how is one of JP_SHORT, JP_MEDIUM, JP_LONG from here */
    j->flags &= ~JF_CHANGED;
    filler = j->job > 10 ? "\n       " : "\n      ";
    if (j == job_list)
        jobchar = '+';
    else if (j == job_list->next)
        jobchar = '-';
    else
        jobchar = ' ';

    for (p = j->proc_list; p != NULL;) {
#ifdef WCOREDUMP
        coredumped = false;
#endif
        switch (p->state) {
        case PRUNNING:
            msg = "Running";
            break;
        case PSTOPPED:
            status = WSTOPSIG(p->status);
            msg = status > 0 && status < qsh_NSIG ? sigtraps[status].mess : "Stopped";
            break;
        case PEXITED:
            if (how == JP_SHORT)
                msg = null;
            else if ((status = (WEXITSTATUS(p->status)) & 255) == 0)
                msg = "Done";
            else {
                shf_snprintf(msgbuf, sizeof(msgbuf), TDone, status);
                msg = msgbuf;
            }
            break;
        case PSIGNALLED:
#ifdef WCOREDUMP
            if (WCOREDUMP(p->status))
                coredumped = true;
#endif
            status = WTERMSIG(p->status);
            /* only report “abnormal” termination signals short */
            if (how != JP_SHORT ||
#ifdef WCOREDUMP
                coredumped ||
#endif
                (status != SIGINT && status != SIGPIPE)) {
                msg = status > 0 && status < qsh_NSIG ? sigtraps[status].mess : "Signalled";
                break;
            }
            /* FALLTHROUGH */
        default:
            msg = null;
        }

        if (how == JP_SHORT) {
            if (msg[0]) {
                output = true;
                shf_puts(msg, shf);
#ifdef WCOREDUMP
                if (coredumped)
                    shf_puts(" (core dumped)", shf);
#endif
                shf_putc(' ', shf);
            }
        } else {
            /* JP_MEDIUM or JP_LONG */
            if (p == j->proc_list)
                shf_fprintf(shf, "[%d] %c ", j->job, jobchar);
            else
                shf_puts(filler, shf);
            if (how == JP_LONG)
                shf_fprintf(shf, "%5d ", (int)p->pid);
            output = true;
            shf_fprintf(shf, "%-20s %s", msg, p->command);
            if (p->next) {
                shf_putc(' ', shf);
                shf_putc('|', shf);
            }
#ifdef WCOREDUMP
            if (coredumped)
                shf_puts(" (core dumped)", shf);
#endif
        }

        state = p->state;
        status = p->status;
        p = p->next;
        while (p && p->state == state && p->status == status) {
            switch (how) {
            case JP_LONG:
                shf_puts(filler, shf);
                shf_fprintf(shf, "%5d %-20s", (int)p->pid, T1space);
                /* FALLTHROUGH */
            case JP_MEDIUM:
                shf_putc(' ', shf);
                shf_puts(p->command, shf);
                if (p->next) {
                    shf_putc(' ', shf);
                    shf_putc('|', shf);
                }
                break;
            }
            p = p->next;
        }
    }
    if (output)
        shf_putc('\n', shf);
}

/*
 * Convert % sequence to job
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static Job *
j_lookup(const char *cp, int *ecodep)
{
    Job *j, *last_match;
    Proc *p;
    size_t len;
    int job = 0;

    if (ctype(*cp, C_DIGIT) && getn(cp, &job)) {
        /* Look for last_proc->pid (what $! returns) first... */
        for (j = job_list; j != NULL; j = j->next)
            if (j->last_proc && j->last_proc->pid == job)
                return (j);
        /*
         * ...then look for process group (this is non-POSIX,
         * but should not break anything
         */
        for (j = job_list; j != NULL; j = j->next)
            if (j->pgrp && j->pgrp == job)
                return (j);
        goto j_lookup_nosuch;
    }
    if (*cp != '%') {
    j_lookup_invalid:
        if (ecodep)
            *ecodep = JL_INVALID;
        return (NULL);
    }
    switch (*++cp) {
    case '\0': /* non-standard */
    case '+':
    case '%':
        if (job_list != NULL)
            return (job_list);
        break;

    case '-':
        if (job_list != NULL && job_list->next)
            return (job_list->next);
        break;

    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        if (!getn(cp, &job))
            goto j_lookup_invalid;
        for (j = job_list; j != NULL; j = j->next)
            if (j->job == job)
                return (j);
        break;

    /* %?string */
    case '?':
        last_match = NULL;
        for (j = job_list; j != NULL; j = j->next)
            for (p = j->proc_list; p != NULL; p = p->next)
                if (vstrstr(p->command, cp + 1)) {
                    if (last_match) {
                        if (ecodep)
                            *ecodep = JL_AMBIG;
                        return (NULL);
                    }
                    last_match = j;
                }
        if (last_match)
            return (last_match);
        break;

    /* %string */
    default:
        len = strlen(cp);
        last_match = NULL;
        for (j = job_list; j != NULL; j = j->next)
            if (strncmp(cp, j->proc_list->command, len) == 0) {
                if (last_match) {
                    if (ecodep)
                        *ecodep = JL_AMBIG;
                    return (NULL);
                }
                last_match = j;
            }
        if (last_match)
            return (last_match);
        break;
    }
j_lookup_nosuch:
    if (ecodep)
        *ecodep = JL_NOSUCH;
    return (NULL);
}

static Job *free_jobs;
static Proc *free_procs;

/*
 * allocate a new job and fill in the job number.
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static Job *
new_job(void)
{
    int i;
    Job *newj, *j;

    if (free_jobs != NULL) {
        newj = free_jobs;
        free_jobs = free_jobs->next;
    } else {
        char *cp;

        /*
         * struct job includes ALLOC_ITEM for alignment constraints
         * so first get the actually used memory, then assign it
         */
        cp = alloc(sizeof(Job) - sizeof(ALLOC_ITEM), APERM);
        /* undo what alloc() did to the malloc result address */
        newj = (void *)(cp - sizeof(ALLOC_ITEM));
    }

    /* brute force method */
    i = 0;
    do {
        ++i;
        j = job_list;
        while (j && j->job != i)
            j = j->next;
    } while (j);
    newj->job = i;

    return (newj);
}

/*
 * Allocate new process struct
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static Proc *
new_proc(void)
{
    Proc *p;

    if (free_procs != NULL) {
        p = free_procs;
        free_procs = free_procs->next;
    } else
        p = alloc(sizeof(Proc), APERM);

    return (p);
}

/*
 * Take job out of job_list and put old structures into free list.
 * Keeps nzombies, last_job and async_job up to date.
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static void
remove_job(Job *j, const char *where)
{
    Proc *p, *tmp;
    Job **prev, *curr;

    prev = &job_list;
    curr = job_list;
    while (curr && curr != j) {
        prev = &curr->next;
        curr = *prev;
    }
    if (curr != j) {
        kwarnf0(KWF_INTERNAL | KWF_WARNING | KWF_NOERRNO, "remove_job: job %s (%s)", Tnot_found,
                where);
        return;
    }
    *prev = curr->next;

    /* free up proc structures */
    for (p = j->proc_list; p != NULL;) {
        tmp = p;
        p = p->next;
        tmp->next = free_procs;
        free_procs = tmp;
    }

    if ((j->flags & JF_ZOMBIE) && j->ppid == procpid)
        --nzombie;
    j->next = free_jobs;
    free_jobs = j;

    if (j == last_job)
        last_job = NULL;
    if (j == async_job)
        async_job = NULL;
}

/*
 * put j in a particular location (taking it out job_list if it is there
 * already)
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static void
put_job(Job *j, int where)
{
    Job **prev, *curr;

    /* Remove job from list (if there) */
    prev = &job_list;
    curr = job_list;
    while (curr && curr != j) {
        prev = &curr->next;
        curr = *prev;
    }
    if (curr == j)
        *prev = curr->next;

    switch (where) {
    case PJ_ON_FRONT:
        j->next = job_list;
        job_list = j;
        break;

    case PJ_PAST_STOPPED:
        prev = &job_list;
        curr = job_list;
        for (; curr && curr->state == PSTOPPED; prev = &curr->next, curr = *prev)
            ;
        j->next = curr;
        *prev = j;
        break;
    }
}

/*
 * nuke a job (called when unable to start full job).
 *
 * If jobs are compiled in then this routine expects sigchld to be blocked.
 */
static int
kill_job(Job *j, int sig)
{
    Proc *p;
    int rval = 0;

    for (p = j->proc_list; p != NULL; p = p->next)
        if (p->pid != 0)
            if (kill(p->pid, sig) < 0)
                rval = -1;
    return (rval);
}

static void
tty_init_talking(void)
{
    switch (tty_init_fd()) {
    case 0:
        break;
    case 1:
        break;
    case 2:
        break;
    case 3:
        kwarnf0(KWF_PREFIX, Tf_ssfailed, "j_ttyinit", "dup of tty fd");
        break;
    case 4:
        kwarnf(KWF_PREFIX | KWF_TWOMSG, "j_ttyinit", "can't set close-on-exec flag");
        break;
    }
}

static void
vistree(char *dst, size_t sz, struct op *t)
{
    char buf[PROC_TGTSZ - 12];
    struct shf shf;

    snptreef(buf, sizeof(buf), Tf_T, t);
    shf_sopen(dst, sz, SHF_WR, &shf);
    uprntmbs(buf, false, &shf);
    while ((char *)shf.wp > dst && ctype(shf.wp[-1], C_IFSWS))
        --shf.wp;
    shf_sclose(&shf);
}
