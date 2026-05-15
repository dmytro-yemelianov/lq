/*
 * <qsoe/wire.h> — internal wire protocol between libqsoe and taskman.
 *
 * Shared between libqsoe (request side) and taskman (handler side) so
 * message-label numbers and MR layout stay in sync.  Not part of the
 * public API surface — application code should never include this.
 *
 * v0.7 bucketed labels — each subsystem in taskman owns a fixed slice
 * of the 8-bit label space so we can grep by range:
 *
 *    0x00..0x3F  sysmgr   (system services: debug, ping, future ioport
 *                          / sysinfo / shutdown)
 *    0x40..0x7F  procmgr  (processes / threads / channels / connections
 *                          / pulses / signals / cred + ppid query)
 *    0x80..0xBF  memmgr   (mmap / future munmap, mprotect, …)
 *    0xC0..0xEF  pathmgr  (pathmgr_register / repath + open / close /
 *                          io read / io write — every fd-flavoured op)
 *    0xF0..0xFF  reserved
 *
 * See Design doc §4.4 "Wire protocol".
 */
#ifndef QSOE_WIRE_H
#define QSOE_WIRE_H

enum {
    /* ---------- sysmgr (0x00..0x3F) ---------- */
    TM_REQ_DEBUG_SLOT_COUNT     = 0x01,  /* cap-leak smoke test */
    TM_REQ_PING_CLIENTINFO      = 0x02,  /* server-side CCI demo / smoke */
    /* v0.7 platform timer frequency (Hz).  Reply mr0 = ticks per
     * second of RISC-V's `time` CSR (what `rdtime` returns).  libqsoe
     * queries this once at process startup and caches it in the
     * global `qsoe_time_freq_hz`, so subsequent ClockTime / nanosleep
     * / etc. read rdtime directly and convert without IPC. */
    TM_REQ_CLOCK_FREQ           = 0x03,

    /* ---------- procmgr (0x40..0x7F) ---------- */
    TM_REQ_CHANNEL_CREATE       = 0x40,
    TM_REQ_CHANNEL_DESTROY      = 0x41,
    TM_REQ_CONNECT_ATTACH       = 0x42,
    TM_REQ_CONNECT_DETACH       = 0x43,
    TM_REQ_CONNECT_SERVER_INFO  = 0x44,
    TM_REQ_CONNECT_CLIENT_INFO  = 0x45,
    TM_REQ_CONNECT_FLAGS        = 0x46,
    TM_REQ_THREAD_ALLOC         = 0x47,
    TM_REQ_THREAD_DESTROY       = 0x48,
    TM_REQ_PROCESS_CREATE       = 0x49,
    TM_REQ_PROCESS_TERMINATE    = 0x4a,
    TM_REQ_PULSE_SEND           = 0x4b,
    TM_REQ_PULSE_FETCH          = 0x4c,
    TM_REQ_PROC_DETACH          = 0x4d,
    TM_REQ_WAITPID              = 0x4e,
    TM_REQ_REGISTER_SIGNAL_CHID = 0x4f,
    TM_REQ_GET_SIGNAL_CHID      = 0x50,
    /* v0.7: per-process self info — pid, ppid, cred — for POSIX
     * getpid / getppid / getuid / geteuid / getgid / getegid.  No
     * arguments; caller pid comes from the badge.  Reply layout
     * documented in libqsoe/src/proc_info.c. */
    TM_REQ_PROC_SELF_INFO       = 0x51,
    /* v0.7 cwd accessors.  CHDIR: caller's path in msg[4..], path_len
     *  in MR0.  GETCWD: reply len in MR0, bytes in msg[4..]. */
    TM_REQ_CHDIR                = 0x52,
    TM_REQ_GETCWD               = 0x53,
    /* v0.7 cred mutation backing setuid/setgid/setresuid/setresgid/etc.
     * MR0 packs (ruid<<32 | euid), MR1 packs (suid<<32 | rgid), MR2
     * packs (egid<<32 | sgid).  A 32-bit field with value
     * 0xFFFFFFFF means "don't change".  v0.7 has no privilege check
     * (everyone is root); v0.8+ verifies caller's euid before
     * applying. */
    TM_REQ_SET_CRED             = 0x56,
    /* v0.7 dup-cap: copy a cap inside the caller's own CSpace.
     *   MR0 = src slot, MR1 = dest slot (both in caller's CSpace).
     *   Backs POSIX dup2 / F_DUPFD. */
    TM_REQ_DUP_CAP              = 0x54,
    /* v0.7 per-process file-creation mask.
     *   GET: no args, reply mr0 = old mask.
     *   SET: MR0 = new mask (low 9 bits), reply mr0 = old mask. */
    TM_REQ_UMASK                = 0x55,

    /* ---------- memmgr (0x80..0xBF) ---------- */
    /* MR0 = length (bytes; rounded up to 2 MiB by taskman).
     * Reply: MR0 = base vaddr.  Label = ENOMEM on failure. */
    TM_REQ_MMAP                 = 0x80,

    /* ---------- pathmgr (0xC0..0xEF) ---------- */
    /* Runtime mutation: REGISTER announces a resmgr at a path
     * (path_len in MR0, chid in MR1; resmgr pid from caller badge).
     * REPATH retargets an existing path entry (used by init to swap
     * /dev/console from in-taskman to a real UART driver). */
    TM_REQ_PATHMGR_REGISTER     = 0xC0,
    TM_REQ_PATHMGR_REPATH       = 0xC1,
    /* POSIX-side fd surface — open() consults the path manager;
     * read / write / close operate on a connection (badge identifies it). */
    TM_REQ_OPEN                 = 0xC2,
    TM_REQ_CLOSE                = 0xC3,
    TM_REQ_IO_WRITE             = 0xC4,
    TM_REQ_IO_READ              = 0xC5,
    /* v0.7 file-side ops: UNLINK takes a path in msg[4..] (path_len
     *  in MR0).  FSTAT operates on the connection (badge identifies
     *  it); reply payload is a tm_stat_t in msg[4..] sized via MR0. */
    TM_REQ_UNLINK               = 0xC6,
    TM_REQ_FSTAT                = 0xC7,
    /* v0.7 readlink: resolve via pathmgr, ask the resmgr to return
     * the link target.  MR0 = path_len (path in msg[4..]); reply
     * mr0 = bytes written; payload in msg[4..].  v0.7 has no
     * symlinks, so successful resolution always yields EINVAL. */
    TM_REQ_READLINK             = 0xC8,
    /* v0.7 lseek: reposition the fd's offset.  Routes through the
     * fd's bound connection (badge identifies the resmgr).
     *   MR0 = whence (0/1/2 = SET/CUR/END), MR1 = signed offset.
     *   Reply mr0 = resulting absolute offset. */
    TM_REQ_LSEEK                = 0xC9,
    /* v0.7 readdir: fetch one directory entry from the connection.
     * Reply payload at msg[4..]: 1 byte d_type, then NUL-terminated
     * name.  MR0 = total byte count.  Label = ENOENT past end. */
    TM_REQ_READDIR              = 0xCA,
    /* v0.7 access: probe a path through pathmgr.  Mode bits are
     * ignored in v0.7 (everyone is root); the answer is solely
     * "does the path exist".  MR0 = path_len (path in msg[4..]).
     * Reply label = 0 / ENOENT / etc. */
    TM_REQ_ACCESS               = 0xCB,
};

/* Reply label conventions: 0 on success, positive QNX errno on failure.
 * (Taskman uses positive values; libqsoe negates only when it stores
 * them in qsoe_errno, since the public surface uses positive errnos.)
 */

#endif /* QSOE_WIRE_H */
