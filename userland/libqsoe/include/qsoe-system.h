/*
 * <qsoe-system.h> — QSOE userland system API (libqsoe public surface).
 *
 * Analogous to QNX's <sys/neutrino.h>: one include gives you the full
 * QNX-shape IPC surface plus QSOE-native extensions — channels, con-
 * nections, messages, threads, processes, file IO, mmap, timers,
 * signals, pathmgr, and line discipline.
 *
 * All entrypoints return -1 on failure and set qsoe_errno to a QNX-
 * compatible value.
 *
 * History: replaces qsoe/qrv.h from v0.3.x — same surface, single
 * top-level header.  Slots / wire / tls headers remain separate for
 * now and may fold in later.
 */
#ifndef QSOE_SYSTEM_H
#define QSOE_SYSTEM_H

#include <qsoe/tls.h>   /* qsoe_errno, qsoe_self_pid, qsoe_ipcbuf, pid_t */

typedef unsigned int  uint32_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;

#define ND_LOCAL_NODE 0

/*
 * Side-channel namespace marker. Set on a coid (or chid) returned by
 * ChannelCreate / ConnectAttach when the QSOE_SIDE_CHANNEL flag is
 * passed. The marker lives in bit 30 so the value stays a positive
 * signed int while remaining unambiguously disjoint from the FD coid
 * range (0..N).
 *
 * Rationale (matches QNX _NTO_SIDE_CHANNEL): in QNX-like systems an
 * `int fd` returned by open() is the same integer as the coid the
 * filesystem-or-resmgr connection lives at. fds 0/1/2 are coids
 * 0/1/2. If the system-process connection were a low-numbered coid it
 * would alias stdin/stdout/stderr. Putting it in side-channel space
 * keeps it out of FD allocation.
 */
#define QSOE_SIDE_CHANNEL   0x40000000u

/* The system process (taskman) sits at a fixed pid/chid and is reached
 * by every process via a pre-bound side-channel coid. spawn.c mints
 * the cap at CSpace slot QSOE_CAP_TASKMAN_EP and the libqsoe init hook
 * binds it to SYSMGR_COID. */
#define SYSMGR_PID          1
#define SYSMGR_CHID         1
#define SYSMGR_COID         ((int)QSOE_SIDE_CHANNEL)
#define SYSMGR_HANDLE       0

/* ConnectAttach / ConnectFlags flag bits (mirrors QRV_FLG_COF_*). */
#define QSOE_COF_CLOEXEC    0x0001u
#define QSOE_COF_DEAD       0x0002u
#define QSOE_COF_NOSHARE    0x0040u
#define QSOE_COF_NONBLOCK   0x0100u

/* Error codes (QNX-compatible subset for v0.x), sorted by value. */
#define EOK             0
#define EPERM           1
#define ENOENT          2
#define ESRCH           3
#define EINTR           4
#define EIO             5
#define E2BIG           7
#define ENOEXEC         8
#define EBADF           9
#define ECHILD         10
#define EAGAIN         11
#define ENOMEM         12
#define EFAULT         14
#define EBUSY          16
#define EEXIST         17
#define ENODEV         19
#define ENOTDIR        20
#define EISDIR         21
#define EINVAL         22
#define EMFILE         24
#define ENOTTY         25
#define EFBIG          27
#define ESPIPE         29
#define EPIPE          32
#define EROFS          30
#define ERANGE         34
#define ENAMETOOLONG   36
#define ENOSYS         89
#define EHOSTUNREACH  113

/* POSIX `struct stat` byte-for-byte for RISC-V64 musl.  Built into
 * the FSTAT reply payload at msg[4..]; libc/qsoe's fstat.c copies
 * these bytes into the user-supplied struct stat.  Field widths are
 * spelled with concrete unsigned long / unsigned int so the wire
 * shape is independent of any libc header.  Lives here (in libqsoe)
 * because resmgrs hand-build it without taskman includes. */
typedef struct {
    unsigned long      st_dev;
    unsigned long      st_ino;
    unsigned int       st_mode;
    unsigned int       st_nlink;
    unsigned int       st_uid;
    unsigned int       st_gid;
    unsigned long      st_rdev;
    unsigned long      __pad;
    long               st_size;
    int                st_blksize;
    int                __pad2;
    long               st_blocks;
    long               st_atim_sec;
    long               st_atim_nsec;
    long               st_mtim_sec;
    long               st_mtim_nsec;
    long               st_ctim_sec;
    long               st_ctim_nsec;
    unsigned int       __unused[2];
} tm_stat_t;

/* Mode bits matching POSIX <sys/stat.h>. */
#define TM_S_IFMT   0170000
#define TM_S_IFREG  0100000
#define TM_S_IFCHR  0020000
#define TM_S_IFDIR  0040000

int ChannelCreate(unsigned flags);
int ChannelDestroy(int chid);
int ConnectAttach(uint32_t nd, pid_t pid, int chid, unsigned index, int flags);
int ConnectDetach(int coid);

/*
 * Message-passing primitives. QNX terminology + semantics:
 *   MsgSend     synchronous send-and-receive; blocks until reply
 *   MsgReceive  block on a channel until a sender shows up
 *   MsgReply    reply to a previously-received message
 *
 * Byte payloads up to 960 bytes (seL4's IPC buffer capacity) fit
 * directly in the IPC buffer. v0.4+ adds shared-frame channels for
 * larger transfers.
 *
 * Non-MCS reply caveat: on seL4 non-MCS the reply capability is a
 * single per-thread slot (tcbCaller). A server MUST reply to each
 * received message before receiving the next, or the older reply cap
 * is dropped. The taskman dispatch loop uses MsgReply followed by
 * MsgReceive (or the merged ReplyRecv path) to honour this. v0.4
 * adds seL4_CNode_SaveCaller for queued/delayed replies.
 */

/* Minimal QNX-compatible _msg_info subset. v0.4 grows the rest. */
struct _msg_info {
    uint32_t nd;        /* sending node (always ND_LOCAL_NODE in v0.x) */
    pid_t    pid;       /* sender's pid (badge, in our mapping) */
    int      chid;      /* channel the message arrived on */
    int      scoid;     /* server-side connection id (== badge for v0.x) */
    int      coid;      /* sender's connection id (unknown server-side) */
    int      msglen;    /* total bytes the sender sent (clamped to buf) */
    int      srcmsglen; /* same as msglen until v0.4 */
    int      dstmsglen; /* requested receive size */
    int      priority;  /* (unused; v0.4) */
    int      flags;     /* QSOE_MI_PULSE if wake came from a pulse */
    unsigned label;     /* QSOE-extension: TM_REQ_* / wire-protocol tag */
};

/* QNX uses `_server_info` and `_msg_info` interchangeably; the former
 * is the field-for-field subset filled by ConnectServerInfo while the
 * latter is what MsgReceive fills. Same shape, single alias. */
#define _server_info _msg_info

struct _cred_info {
    uid_t  ruid, euid, suid;
    gid_t  rgid, egid, sgid;
    uint32_t ngroups;
    /* In QNX a flexible array `gid_t grouplist[ngroups]` follows here;
     * QSOE has no users/groups yet so ngroups is always 0 in v0.3.3. */
};

struct _client_info {
    uint32_t nd;
    pid_t    pid;
    pid_t    sid;
    uint32_t flags;
    struct _cred_info cred;
};

int MsgSend(int coid, const void *smsg, int sbytes,
            void *rmsg, int rbytes);
int MsgReceive(int chid, void *msg, int bytes, struct _msg_info *info);
int MsgReply(int rcvid, int status, const void *msg, int bytes);

/* Save the implicit reply cap so the reply can be deferred across
 * subsequent MsgReceive calls.  Returns a stable rcvid with the
 * QSOE_RCVID_SAVED bit set; pass that to MsgReply when the deferred
 * work completes.  Returns -1 / qsoe_errno=EAGAIN if the per-thread
 * save-slot pool is exhausted.  Used by resmgrs that park clients —
 * a pipe with an empty buffer, a UART driver waiting on RX, etc.
 *
 * GOTCHA: QSOE_RCVID_SAVED is bit 31 of `int`, i.e. the sign bit.  A
 * SUCCESSFUL save returns a NEGATIVE signed integer (the SAVED bit
 * is set on success).  Callers MUST check for failure with
 *
 *      if (saved == -1) { ... handle EAGAIN ... }
 *
 * and NEVER with `if (saved < 0)` — that would treat every successful
 * park as a failure.  This is documented because we already got bitten
 * by it once in the devc-ser8250 rework. */
#define QSOE_RCVID_SAVED  0x80000000
int MsgSavereply(int rcvid);

/*
 * Pulses (v0.4.2). Async fixed-size messages: 8-bit signed code
 * + 32-bit value. Queued at the target channel; receiver picks them
 * up via MsgReceive alongside regular messages.
 *
 * Layout matches QNX's struct _pulse. type == _PULSE_TYPE marks
 * application pulses; v0.5+ adds kernel-defined subtypes for
 * signal/timer delivery etc.
 *
 * MsgReceive sets _msg_info.flags |= QSOE_MI_PULSE when it returns a
 * pulse rather than a regular message, and writes the _pulse struct
 * into the receiver's msg buffer. The receive function still returns
 * the rcvid; for pulses the rcvid is "no reply expected" but we
 * return the sender's pid for symmetry.
 */
#define _PULSE_TYPE       0
#define QSOE_MI_PULSE     0x00000010u   /* _msg_info.flags bit */

/* v0.4.3: a bit set on the badge of the Notification cap taskman mints
 * for each pulse-bearing channel. EP-cap badges in QSOE encode the
 * caller's pid (≤ 255), so bit 63 is always free for marking "this
 * Recv wake came from the bound Notification, not an EP message".
 * libqsoe's MsgReceive checks this on every wake to decide whether to
 * fetch a queued pulse from taskman or process an EP message. */
#define QSOE_NTFN_BADGE_BIT  (1UL << 63)

typedef int  int32_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef unsigned long uint64_t;     /* RISC-V64 LP64 */
typedef signed char  int8_t;
typedef unsigned char uint8_t;

struct _pulse {
    uint16_t type;       /* _PULSE_TYPE for application pulses */
    uint16_t subtype;    /* 0 for app pulses */
    int8_t   code;       /* user-defined, signed 8-bit */
    uint8_t  reserved[3];
    union {
        int32_t  sival_int;
        void    *sival_ptr;
    } value;
    int32_t  scoid;      /* taskman's view of the sender connection */
};

int MsgSendPulse(int coid, int priority, int code, int value);

/*
 * Connection introspection / control.
 *
 *   ConnectServerInfo  — client side: who is on the other end of `coid`?
 *                        Fills server's nd/pid/chid/scoid; coid echoes
 *                        the argument. v0.3.3 requires pid==0
 *                        (caller's own process); ENOSYS otherwise.
 *
 *   ConnectClientInfo  — server side: who is the client behind `scoid`
 *                        (the value MsgReceive returned in
 *                        info->scoid)? Fills client's nd/pid/sid/flags;
 *                        cred is zeroed in v0.3.3 (no users yet).
 *                        `ngroups` is the caller's grouplist[] capacity;
 *                        ignored until v0.4+ adds groups.
 *
 *   ConnectFlags       — atomic flag get/set on a connection.
 *                        mask==0 → pure query (no mutation). Returns
 *                        the *previous* flag value, or -1 on error.
 */
int ConnectServerInfo(pid_t pid, int coid, struct _server_info *info);
int ConnectClientInfo(int scoid, struct _client_info *info, int ngroups);

/* v0.7: caller's own (pid, ppid, cred).  Backs POSIX
 * getpid / getppid / getuid / geteuid / getgid / getegid via libc/qsoe. */
int qsoe_proc_self_info(pid_t *out_pid, pid_t *out_ppid,
                        struct _cred_info *out_cred);

/* ============== QNX-compatible Clock / Timer core ============== */

/* Types + CLOCK_* constants are guarded so QSOE-side translation units
 * that also pull in musl's <time.h> don't trip on the duplicate
 * definitions.  Values mirror musl's bits/alltypes.h / time.h so a
 * single integer answers in both worlds. */
#ifndef __DEFINED_clockid_t
typedef int clockid_t;
#define __DEFINED_clockid_t
#endif
#ifndef __DEFINED_timer_t
typedef void *timer_t;
#define __DEFINED_timer_t
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME             0
#define CLOCK_MONOTONIC            1
#define CLOCK_PROCESS_CPUTIME_ID   2
#define CLOCK_THREAD_CPUTIME_ID    3
#endif

struct _clockperiod {
    unsigned int nsec;
    int          fract;   /* fractional nsec; 64-bit fixed-point */
};

struct _clockadjust {
    unsigned long tick_count;
    long          tick_nsec_inc;
};

/* Set in _qsoe_start_main from TM_REQ_CLOCK_FREQ (in taskman, set
 * directly from TM_CLOCK_FREQ_HZ).  Read by ClockTime / ClockCycles
 * / nanosleep / etc.  Hz: ticks per second of RISC-V `rdtime`. */
extern unsigned long qsoe_time_freq_hz;

int ClockTime  (clockid_t id, const unsigned long *new_, unsigned long *old);
int ClockAdjust(clockid_t id, const struct _clockadjust *new_, struct _clockadjust *old);
int ClockPeriod(clockid_t id, const struct _clockperiod *new_, struct _clockperiod *old, int reserved);
int ClockId    (pid_t pid, int tid);
unsigned long ClockCycles(void);
int ConnectFlags(pid_t pid, int coid, unsigned mask, unsigned bits);

/*
 * Threading (v0.4). QNX/QRV-compatible surface.
 *
 *   ThreadCreate(pid, func, arg, attr) — spawn a new thread in the
 *     given process. pid==0 means "this process"; cross-process
 *     ThreadCreate returns ENOSYS in v0.4. Returns the new tid (>= 2)
 *     or -1.
 *
 *   ThreadDestroy(tid, prio, status) — terminate a thread, set its
 *     exit_status (read by ThreadJoin), revoke its TCB. Calling with
 *     tid==0 means "current thread"; the trampoline does this on
 *     return from the user func.
 *
 *   ThreadDetach(tid) — set the detached flag; once set, the thread's
 *     resources are reclaimed when it exits and ThreadJoin fails.
 *
 *   ThreadJoin(tid, *status) — block until the thread exits; receive
 *     its exit_status.
 *
 *   ThreadCancel(tid, canstub) — request termination. v0.4 supports
 *     only deferred cancellation: the target observes cancel_pending
 *     at the next libqsoe entry point (MsgSend / MsgReceive / etc).
 *     `canstub` is stored but unused in v0.4.
 *
 *   ThreadCtl(cmd, data) — control op. v0.4 supports QSOE_TCTL_NAME
 *     and QSOE_TCTL_RUNMASK; others return ENOSYS.
 */
typedef unsigned long size_t;

#define QSOE_PTHREAD_CREATE_JOINABLE  0
#define QSOE_PTHREAD_CREATE_DETACHED  1

struct _thread_attr {
    int        flags;       /* QSOE_PTHREAD_CREATE_DETACHED bit */
    size_t     stacksize;   /* if 0 → libqsoe default (56 KiB) */
    void      *stackaddr;   /* if NULL → libqsoe allocates */
    void     (*exitfunc)(void *status);   /* stored, not yet wired in v0.4 */
    int        policy;
    int        prio;        /* 1..255; 0 → default 254 */
    unsigned   runmask;     /* SMP affinity bitmask; 0 → CPU 0 */
    unsigned   guardsize;
    unsigned   prealloc;
};

#define QSOE_TCTL_NAME     11
#define QSOE_TCTL_RUNMASK   4

int ThreadCreate(pid_t pid, void *(*func)(void *), void *arg,
                 const struct _thread_attr *attr);
int ThreadDestroy(int tid, int priority, void *status);
int ThreadDetach(int tid);
int ThreadJoin(int tid, void **status);
int ThreadCancel(int tid, void (*canstub)(void));
int ThreadCtl(int cmd, void *data);

/*
 * Process lifecycle (v0.4.1). The minimal QSOE surface; full QNX
 * compatibility (file_actions, attr struct, argv/envp delivery) lands
 * incrementally.
 *
 *   ProcessCreate(path) — spawn the named ELF (looked up in the
 *     embedded userland CPIO for v0.4.1). Returns the new pid or -1.
 *
 *   posix_spawn — POSIX-conformant wrapper. Returns 0 on success and
 *     writes the new pid through *pid_out; non-zero errno on failure.
 *     v0.4.1 ignores file_actions, attr, argv, envp.
 *
 *   ProcessTerminate(pid, status) — destroy the named process. pid==0
 *     means "this process" (the no-return self path).
 *
 *   exit / _exit — self-terminate with `status`. POSIX semantics:
 *     exit() runs atexit handlers first (no atexit yet in v0.4.1);
 *     _exit() goes directly to ProcessTerminate(0, status).
 */
int  ProcessCreate(const char *path);
/* posix_spawn is declared by <spawn.h>; libqsoe.a provides the symbol. */

/* POSIX IO entry points (open / close / read / write / writev) now
 * live in userland/libc/qsoe/ and are resolved through libc.a — no
 * qsoe_* shim here. */
int  ProcessTerminate(pid_t pid, int status);
void _exit(int status) __attribute__((noreturn));
void exit (int status) __attribute__((noreturn));

/* v0.6.1 procmgr_detach / waitpid (QNX/QRV-style "stay resident").
 *
 *   procmgr_detach(status) — call from a daemon's main() once it's
 *     ready to serve. taskman delivers `status` to the parent's
 *     parked waitpid() and reparents this process to pid 1 (taskman),
 *     after which this thread keeps running.
 *
 *   waitpid(child_pid, *status, 0) — block until `child_pid` calls
 *     procmgr_detach() or exits. *status receives the value the
 *     child supplied. Returns child_pid on success, -1 on error.
 *     (v0.6.1 ignores the options arg; WNOHANG is v0.7+.)
 */
int  procmgr_detach(int status);
int  waitpid(pid_t pid, int *status, int options);

/* v0.6.1: path-manager mutation primitives.
 *
 * qsoe_pathmgr_register(path, chid) — a resmgr announces that any
 *   open(path) should ConnectAttach to (self_pid, chid). The
 *   announcing process IS the resmgr; taskman picks up its pid
 *   from the badge.
 *
 * qsoe_pathmgr_repath(path, new_pid, new_chid, handler_kind) —
 *   rewrite an already-registered entry to point somewhere else.
 *   Used by init to swap /dev/console after the real UART driver
 *   comes up. handler_kind=0 for external (the normal case);
 *   1/2 for the in-taskman special cases. */
int qsoe_pathmgr_register(const char *path, int chid);
int qsoe_pathmgr_repath  (const char *path, pid_t new_pid,
                          int new_chid, unsigned handler_kind);
/* Longest-prefix resolve.  On success, fills *out_pid / *out_chid /
 * *out_kind with the server binding at the deepest matching prefix.
 * Returns 0; sets qsoe_errno+returns -1 on miss. */
int qsoe_pathmgr_resolve (const char *path, pid_t *out_pid,
                          int *out_chid, unsigned *out_kind);

/* ---------------------------------------------------------------------
 * Interrupt-handling surface (drivers only).
 * ---------------------------------------------------------------------
 *
 * QSOE userland drivers attach an IRQHandler cap to a Notification at
 * boot, then a dedicated IRQ thread blocks in qsoe_irq_wait().  The
 * kernel signals the Notification on each rising edge of the device's
 * PLIC line; the thread drains the device, qsoe_irq_ack()s the handler
 * to re-arm, and loops.  spawn.c pre-mints the IRQHandler and the
 * Notification into well-known caller slots (QSOE_CAP_IRQ_HANDLER /
 * QSOE_CAP_IRQ_NTFN) per driver, so resmgrs only ever see slot ids,
 * never raw seL4 surface.
 */

/* Attach `ntfn` to `handler` so that subsequent device IRQs signal
 * the notification.  Returns 0 on success, -1 / qsoe_errno on failure. */
int qsoe_irq_set_notification(int handler, int ntfn);

/* Block on the IRQ Notification until the kernel signals it.  Used
 * by interrupt-thread loops; returns when an edge has been delivered.
 * Returns 0 always (the wait itself has no failure mode). */
int qsoe_irq_wait(int ntfn);

/* Tell the kernel the IRQ has been serviced and re-arm `handler`. */
int qsoe_irq_ack(int handler);

/*
 * libqsoe init hook. Each spawned process calls this exactly once at
 * startup with its IPC-buffer pointer and pid:
 *   - records the pointer so msg.c can pack into the buffer;
 *   - records self_pid for ConnectServerInfo's pid==0 shortcut;
 *   - for non-taskman processes, pre-binds SYSMGR_COID → CSpace slot
 *     QSOE_CAP_TASKMAN_EP (which spawn.c minted at child startup).
 */
void qsoe_libqsoe_init(void *ipcbuf, pid_t self_pid);

/* ---------------------------------------------------------------------
 * Line discipline (LDISC) — shared between getty / login / qsh.
 * ---------------------------------------------------------------------
 *
 * Slim cooked-mode helper around a byte-stream fd (typically a UART
 * driver registered at /dev/console or /dev/ser*).  Per-instance handle,
 * caller owns the fd.  Intentionally NOT a POSIX termios shim — that
 * lives at a higher layer and lands in v0.8.
 *
 * Backspace accepts both 0x08 (BS) and 0x7F (DEL) regardless of
 * `verase`.  Echo emits "\b \b" on erase if `echoe` is set, plain "\b"
 * if only `echo` is set, nothing otherwise.  In canonical mode the
 * line buffer is filled until a newline arrives, then the whole line
 * (including the trailing '\n') is returned.  In raw mode each byte
 * is passed through verbatim.
 *
 * VINTR (default ^C) aborts an in-flight readline with errno=EINTR
 * when `isig` is set.  Real signal delivery (POSIX SIGINT to the
 * process group) is additive in v0.8.
 */

typedef struct {
    unsigned icanon:1;   /* canonical / cooked mode                 */
    unsigned echo  :1;   /* echo typed chars                        */
    unsigned echoe :1;   /* echo erase as "\b \b" instead of "\b"   */
    unsigned isig  :1;   /* VINTR / VQUIT abort readline (EINTR)    */
    unsigned icrnl :1;   /* translate input CR to NL                */
    unsigned opost :1;   /* perform output processing               */
    unsigned onlcr :1;   /* translate output NL to CR-NL            */
    unsigned char vintr;  /* default 0x03 (^C) */
    unsigned char verase; /* default 0x7F (DEL) — 0x08 (BS) also accepted */
    unsigned char vkill;  /* default 0x15 (^U) — erase to start of line */
    unsigned char veof;   /* default 0x04 (^D) — end-of-file marker */
} qsoe_ldisc_attr_t;

/* Opaque handle. */
typedef struct qsoe_ldisc qsoe_ldisc_t;

/* Open a handle that reads typed bytes from `fd_in` and writes echo /
 * erase / line-feed visuals to `fd_out`.  Pass the same fd for both
 * if the underlying device is bidirectional and you genuinely want
 * one descriptor (rare — terminals in QSOE keep stdin and stdout
 * separate, so the typical call is `qsoe_ldisc_open(0, 1, NULL)`).
 * If `attr` is NULL, defaults are used (canonical, echo, echoe, isig,
 * icrnl, opost, onlcr; VINTR=^C, VERASE=DEL, VKILL=^U, VEOF=^D).
 * Returns NULL on alloc failure.  Does NOT take ownership of either fd. */
qsoe_ldisc_t *qsoe_ldisc_open(int fd_in, int fd_out,
                              const qsoe_ldisc_attr_t *attr);

/* Tear the handle down.  Caller still owns `fd`; ldisc_close does
 * NOT close it. */
void qsoe_ldisc_close(qsoe_ldisc_t *ld);

/* Read / write attributes. */
int qsoe_ldisc_get(qsoe_ldisc_t *ld, qsoe_ldisc_attr_t *out);
int qsoe_ldisc_set(qsoe_ldisc_t *ld, const qsoe_ldisc_attr_t *in);

/* Convenience: switch the whole flag set between cooked and raw. */
int qsoe_ldisc_set_cooked(qsoe_ldisc_t *ld);
int qsoe_ldisc_set_raw   (qsoe_ldisc_t *ld);

/* Read one cooked-mode line into `buf` (terminated by '\n', count
 * includes the '\n').  Returns the byte count; 0 on EOF (VEOF on an
 * empty line); -1 with qsoe_errno=EINTR if isig+VINTR fires. */
long qsoe_ldisc_readline(qsoe_ldisc_t *ld, char *buf, unsigned long cap);

/* Read one byte (no editing, no echo).  Returns 1 on success,
 * 0 on EOF, -1 on error. */
long qsoe_ldisc_readbyte(qsoe_ldisc_t *ld, unsigned char *c);

/* Write `n` bytes through the handle.  If opost+onlcr are set, '\n'
 * is expanded to "\r\n" before transmission. */
long qsoe_ldisc_write(qsoe_ldisc_t *ld, const void *buf, unsigned long n);

#endif /* QSOE_SYSTEM_H */
