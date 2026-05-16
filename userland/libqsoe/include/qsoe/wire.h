/*
 * <qsoe/wire.h> — internal wire protocol between libqsoe and taskman.
 *
 * Shared between libqsoe (request side) and taskman (handler side) so
 * message-label numbers and MR layout stay in sync.  Not part of the
 * public API surface — application code should never include this.
 *
 * Bucketed labels — each subsystem in taskman owns a 256-entry slice
 * of the label space (each label is one entry in seL4's 52-bit
 * MessageInfo label field, so 256 per bucket is room to grow without
 * crowding):
 *
 *    0x000..0x0FF  sysmgr   (system services: debug, ping, clock_freq,
 *                            future ioport / sysinfo / shutdown)
 *    0x100..0x1FF  procmgr  (processes / threads / channels / connections
 *                            / pulses / signals / cred + ppid query +
 *                            pipe_create + dup_cap + umask)
 *    0x200..0x2FF  memmgr   (mmap / future munmap, mprotect, …)
 *    0x300..0x3FF  pathmgr  (pathmgr_register / repath + open / close /
 *                            io read / io write — every fd-flavoured op,
 *                            plus path-side stat / unlink / readlink /
 *                            lseek / readdir / access)
 *    0x400..       reserved
 *
 * Renumbered from the 0x40-wide bucket layout (v0.7-rc1) at Yuri's
 * direction to give each subsystem 256 entries of headroom — 4× the
 * earlier 64-entry slice.  See Design doc §4.4 "Wire protocol".
 */
#ifndef QSOE_WIRE_H
#define QSOE_WIRE_H

enum {
    /* ---------- sysmgr (0x000..0x0FF) ---------- */
    TM_REQ_DEBUG_SLOT_COUNT     = 0x001,  /* cap-leak smoke test */
    TM_REQ_PING_CLIENTINFO      = 0x002,  /* server-side CCI demo / smoke */
    /* RISC-V `time` CSR frequency.  Reply mr0 = ticks per second of
     * what `rdtime` returns.  libqsoe queries this once at process
     * startup and caches it in qsoe_time_freq_hz so subsequent
     * ClockTime / nanosleep / etc. read rdtime directly. */
    TM_REQ_CLOCK_FREQ           = 0x003,

    /* v0.8: full syscfg blob (machine model, CPUs, memory, PCI windows,
     * etc.) built from the FDT at boot.  MR0 = max bytes the caller can
     * accept; reply MR0 = bytes copied into msg[4..].  Label = 0 / EINVAL
     * (caller buffer too small) / ENOSYS (syscfg not built yet). */
    TM_REQ_GET_SYSCFG           = 0x004,

    /* ---------- procmgr (0x100..0x1FF) ---------- */
    TM_REQ_CHANNEL_CREATE       = 0x100,
    TM_REQ_CHANNEL_DESTROY      = 0x101,
    TM_REQ_CONNECT_ATTACH       = 0x102,
    TM_REQ_CONNECT_DETACH       = 0x103,
    TM_REQ_CONNECT_SERVER_INFO  = 0x104,
    TM_REQ_CONNECT_CLIENT_INFO  = 0x105,
    TM_REQ_CONNECT_FLAGS        = 0x106,
    TM_REQ_THREAD_ALLOC         = 0x107,
    TM_REQ_THREAD_DESTROY       = 0x108,
    TM_REQ_PROCESS_CREATE       = 0x109,
    TM_REQ_PROCESS_TERMINATE    = 0x10a,
    TM_REQ_PULSE_SEND           = 0x10b,
    TM_REQ_PULSE_FETCH          = 0x10c,
    TM_REQ_PROC_DETACH          = 0x10d,
    TM_REQ_WAITPID              = 0x10e,
    TM_REQ_REGISTER_SIGNAL_CHID = 0x10f,
    TM_REQ_GET_SIGNAL_CHID      = 0x110,
    /* Per-process self info — pid, ppid, cred — for POSIX
     * getpid / getppid / getuid / geteuid / getgid / getegid.  No
     * arguments; caller pid comes from the badge.  Reply layout
     * documented in libqsoe/src/proc_info.c. */
    TM_REQ_PROC_SELF_INFO       = 0x111,
    /* cwd accessors.  CHDIR: path bytes in msg[4..], path_len in MR0.
     * GETCWD: reply len in MR0, bytes in msg[4..]. */
    TM_REQ_CHDIR                = 0x112,
    TM_REQ_GETCWD               = 0x113,
    /* dup-cap: copy a cap inside the caller's own CSpace.
     *   MR0 = src slot, MR1 = dest slot (both in caller's CSpace).
     *   Backs POSIX dup2 / F_DUPFD. */
    TM_REQ_DUP_CAP              = 0x114,
    /* Per-process file-creation mask.
     *   GET: MR0 = -1, reply mr0 = old mask.
     *   SET: MR0 = new mask (low 9 bits), reply mr0 = old mask. */
    TM_REQ_UMASK                = 0x115,
    /* Cred mutation backing setuid/setgid/setresuid/setresgid/etc.
     * MR0 packs (ruid | euid<<32), MR1 packs (suid | rgid<<32), MR2
     * packs (egid | sgid<<32).  A 32-bit field with value
     * 0xFFFFFFFF means "don't change".  v0.7 has no privilege check
     * (everyone is root); v0.8+ verifies caller's euid before
     * applying. */
    TM_REQ_SET_CRED             = 0x116,
    /* pipe(2) — anonymous pipe.  taskman locates /dev/pipe via
     * pathmgr, allocates a fresh unique pipe id, mints two badged
     * Send-caps onto pipe-mgr's channel into the caller's CSpace.
     *   Reply: MR0 = read-end slot, MR1 = write-end slot.
     * Badge of each cap encodes (uid << 1) | dir_bit so the pipe
     * manager routes IO_READ / IO_WRITE / CLOSE correctly. */
    TM_REQ_PIPE_CREATE          = 0x117,
    /* DETACH_CAP — second half of POSIX close(2).  After libc has
     * sent TM_REQ_CLOSE on the fd's bound cap (the resmgr notifies
     * itself: decrements counts, frees per-fd state), libc calls
     * here to ask taskman to delete the cap from the caller's
     * CSpace and free the connection-table entry.  Two-step close
     * (notify-then-detach) lets external resmgrs see closes
     * uniformly with internal ones.
     *   MR0 = caller-CSpace slot of the cap being dropped. */
    TM_REQ_DETACH_CAP           = 0x118,
    /* NANOSLEEP — blocking sleep, lazy expiry.  MR0 = total
     * nanoseconds.  taskman SaveCaller's the reply slot, parks
     * the caller in g_sleepers[], and replies later from the
     * timer-sweep that runs at every dispatch entry. */
    TM_REQ_NANOSLEEP            = 0x119,
    /* SETITIMER — arm / disarm the per-process ITIMER_REAL alarm.
     *   MR0 = packed (which | (interval_us << 32)).  Which: 0=REAL,
     *         1=VIRTUAL (rejected v0.7), 2=PROF (rejected v0.7).
     *   MR1 = initial value in microseconds (0 disarms).
     *   MR2 = interval in microseconds (0 = one-shot).
     *   Reply MR0 = old initial value (us), MR1 = old interval (us). */
    TM_REQ_SETITIMER            = 0x11a,

    /* ---------- memmgr (0x200..0x2FF) ---------- */
    /* MR0 = length (bytes; rounded up to 2 MiB by taskman).
     * Reply: MR0 = base vaddr.  Label = ENOMEM on failure. */
    TM_REQ_MMAP                 = 0x200,

    /* ---------- pathmgr (0x300..0x3FF) ---------- */
    /* Runtime mutation: REGISTER announces a resmgr at a path
     * (path_len in MR0, chid in MR1; resmgr pid from caller badge).
     * REPATH retargets an existing path entry (used by init to swap
     * /dev/console from in-taskman to a real UART driver). */
    TM_REQ_PATHMGR_REGISTER     = 0x300,
    TM_REQ_PATHMGR_REPATH       = 0x301,
    /* POSIX fd surface — open() consults the path manager; read /
     * write / close operate on a connection (badge identifies it). */
    TM_REQ_OPEN                 = 0x302,
    TM_REQ_CLOSE                = 0x303,
    TM_REQ_IO_WRITE             = 0x304,
    TM_REQ_IO_READ              = 0x305,
    /* UNLINK takes a path in msg[4..] (path_len in MR0).  FSTAT
     * operates on the connection (badge identifies it); reply
     * payload is a tm_stat_t in msg[4..] sized via MR0. */
    TM_REQ_UNLINK               = 0x306,
    TM_REQ_FSTAT                = 0x307,
    /* readlink: resolve via pathmgr, ask the resmgr to return the
     * link target.  MR0 = path_len (path in msg[4..]); reply
     * mr0 = bytes written; payload in msg[4..].  v0.7 has no
     * symlinks, so successful resolution yields EINVAL. */
    TM_REQ_READLINK             = 0x308,
    /* lseek: reposition the fd's offset.  Routes through the fd's
     * bound connection.  MR0 = whence (0/1/2 = SET/CUR/END), MR1 =
     * signed offset.  Reply mr0 = resulting absolute offset. */
    TM_REQ_LSEEK                = 0x309,
    /* readdir: fetch one directory entry from the connection.
     * Reply payload at msg[4..]: 1 byte d_type, then NUL-terminated
     * name.  MR0 = total byte count.  Label = ENOENT past end. */
    TM_REQ_READDIR              = 0x30a,
    /* access: probe a path through pathmgr.  Mode bits ignored in
     * v0.7 (everyone is root); the answer is solely "does this
     * path exist".  MR0 = path_len (path in msg[4..]).
     * Reply label = 0 / ENOENT / etc. */
    TM_REQ_ACCESS               = 0x30b,
    /* resolve: longest-prefix lookup on a path.  MR0 = path_len (path
     * in msg[4..]).  Reply MR0 = server_pid, MR1 = server_chid,
     * MR2 = handler_kind, MR3 = bytes consumed by the match.  Label
     * = 0 / ENOENT.  Used by /sbin/repath so it can copy an existing
     * (pid, chid) binding to another path without the caller having
     * to know the driver's pid. */
    TM_REQ_PATHMGR_RESOLVE      = 0x30c,
};

/* Reply label conventions: 0 on success, positive QNX errno on failure.
 * (Taskman uses positive values; libqsoe negates only when it stores
 * them in qsoe_errno, since the public surface uses positive errnos.)
 */

#endif /* QSOE_WIRE_H */
