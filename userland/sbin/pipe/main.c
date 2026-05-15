/*
 * userland/sbin/pipe — POSIX pipe / FIFO resource manager.
 *
 * One userspace process serves /dev/pipe via the path manager.
 * Per QSOE rule (CLAUDE.md "Requirements for resource managers"):
 * pipe touches NO seL4 primitives directly — every IPC, channel
 * creation, and reply is through libqsoe's QNX-shape API.  libc
 * provides everything else (printf, memcpy, ring math).
 *
 * Architecture sketch:
 *
 *     libc pipe(fd[2])   →   TM_REQ_PIPE_CREATE on taskman
 *                              │
 *                              │  (taskman bridges: ChannelCreate
 *                              │   was already done by pipe mgr;
 *                              │   taskman ConnectAttach × 2 with
 *                              │   badges encoding (pipe-id, dir).)
 *                              ▼
 *     pipe manager  ←  reads / writes / closes via IO_READ/WRITE/CLOSE
 *
 * Blocking semantics use the QNX rcvid-park pattern:
 *   * read on empty buffer with active writer → save rcvid, no reply
 *   * write arriving while a parked reader exists → fulfil + reply both
 *   * write to full buffer with active reader  → save rcvid, no reply
 *   * read draining while a parked writer exists → fulfil + reply both
 *
 * Anonymous pipes only in v0.7.  Named FIFOs (mknod-style) need
 * pathmgr-mutation extensions and arrive later.
 *
 * v0.7 scope of THIS file:
 *   * Channel up; pathmgr registration; main MsgReceive loop.
 *   * Real ring buffer (4 KiB) per pipe slot.
 *   * Read/write/close dispatch on the badge → (pipe-id, direction).
 *
 *   The libc pipe(2) wire bridge (TM_REQ_PIPE_CREATE on taskman) and
 *   the per-process spawn-at-boot wiring are next-turn work — once
 *   those land, qsh's `cmd1 | cmd2` lights up.
 */

#include <stdio.h>
#include <string.h>
#include <qsoe/qrv.h>
#include <qsoe/slots.h>
#include <qsoe/wire.h>

/* qsoe_ipcbuf casts to seL4_IPCBuffer* and we need seL4_Word; both
 * come from QSOE's sel4_types.h wrapper.  No direct seL4 syscalls
 * issued from this file (CLAUDE.md "Requirements for resource
 * managers"). */
#include "../../taskman/sel4_types.h"

/* Service registration: announce ourselves at /dev/pipe. */
#define PIPE_MANAGER_PATH "/dev/pipe"
#define PIPE_MANAGER_CHID 1     /* our local chid (libqsoe-allocated below) */

/* Pipe pool — fixed for v0.7; resize when usage proves the cap. */
#define PIPE_POOL_SIZE   16
#define PIPE_BUF_BYTES  4096    /* one page per pipe */

/* Direction bit, encoded in the connection badge by taskman at
 * mint time.  Bit 0 = direction; remaining bits = pipe-id. */
#define PIPE_DIR_BIT     0x1u
#define PIPE_ID_FROM_BADGE(b)  (((unsigned)(b)) >> 1)
#define PIPE_DIR_FROM_BADGE(b) (((unsigned)(b)) & PIPE_DIR_BIT)
#define PIPE_DIR_READ    0u
#define PIPE_DIR_WRITE   1u

typedef struct {
    int       in_use;
    unsigned  unique_id;       /* badge >> 1; taskman-allocated, monotonic */
    unsigned  reader_count;
    unsigned  writer_count;
    /* Ring buffer: producer writes at tail, consumer reads at head.
     * Empty when head == tail; full when (tail + 1) % size == head
     * (one slot left unused, classic Lamport-style ring). */
    unsigned  head;
    unsigned  tail;
    unsigned char buf[PIPE_BUF_BYTES];
    /* Park state — at most one blocked reader and one blocked writer
     * per pipe in v0.7 (single-reader / single-writer pipe is the
     * common shell case).  Multi-waiter queues land when concurrent
     * pipelines become a thing. */
    int       parked_reader_rcvid;
    unsigned  parked_reader_want;
    int       parked_writer_rcvid;
    const unsigned char *parked_writer_buf;
    unsigned  parked_writer_left;
} pipe_t;

static pipe_t g_pipes[PIPE_POOL_SIZE];

/* Find the pipe slot for the given unique_id, allocating lazily on
 * first sight (a pipe end's first IO or first close is what tells
 * us the pipe exists — taskman never notifies us at mint time, by
 * design).  Returns NULL when the pool is exhausted. */
static pipe_t *find_or_alloc_pipe(unsigned uid)
{
    pipe_t *free_slot = 0;
    for (int i = 0; i < PIPE_POOL_SIZE; ++i) {
        if (g_pipes[i].in_use && g_pipes[i].unique_id == uid) return &g_pipes[i];
        if (!g_pipes[i].in_use && !free_slot) free_slot = &g_pipes[i];
    }
    if (!free_slot) return 0;     /* pool exhausted */
    /* Fresh allocation: both ends were minted by taskman before we
     * saw this first IO, so reader_count = writer_count = 1.
     * Subsequent close()s decrement; when both hit 0 we recycle. */
    free_slot->in_use       = 1;
    free_slot->unique_id    = uid;
    free_slot->reader_count = 1;
    free_slot->writer_count = 1;
    free_slot->head         = 0;
    free_slot->tail         = 0;
    free_slot->parked_reader_rcvid = 0;
    free_slot->parked_writer_rcvid = 0;
    free_slot->parked_reader_want  = 0;
    free_slot->parked_writer_left  = 0;
    return free_slot;
}

/* ----------- ring helpers ----------- */

static unsigned ring_count(const pipe_t *p)
{
    return (p->tail + PIPE_BUF_BYTES - p->head) % PIPE_BUF_BYTES;
}

static unsigned ring_space(const pipe_t *p)
{
    /* one slot reserved to distinguish empty from full */
    return (PIPE_BUF_BYTES - 1) - ring_count(p);
}

/* Copy up to `want` bytes from p's ring into `dst`.  Returns bytes
 * actually copied (0 if ring is empty). */
static unsigned ring_drain(pipe_t *p, unsigned char *dst, unsigned want)
{
    unsigned have = ring_count(p);
    unsigned n = want < have ? want : have;
    for (unsigned i = 0; i < n; ++i) {
        dst[i] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_BUF_BYTES;
    }
    return n;
}

/* Copy up to `want` bytes from `src` into p's ring.  Returns bytes
 * actually copied (0 if ring is full). */
static unsigned ring_fill(pipe_t *p, const unsigned char *src, unsigned want)
{
    unsigned room = ring_space(p);
    unsigned n = want < room ? want : room;
    for (unsigned i = 0; i < n; ++i) {
        p->buf[p->tail] = src[i];
        p->tail = (p->tail + 1) % PIPE_BUF_BYTES;
    }
    return n;
}

/* ----------- request handlers ----------- */

/* IO_READ on pipe-id `pid_idx`.  Returns the number of bytes the
 * caller should be told they got; or -1 to indicate "park the
 * caller, no reply yet". */
static long do_read(pipe_t *p, unsigned want, int rcvid)
{
    if (!p->in_use) return -EBADF;
    if (PIPE_BUF_BYTES > 928u && want > 928u) want = 928u;

    unsigned have = ring_count(p);
    if (have > 0) {
        unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
        unsigned got = ring_drain(p, dst, want);
        /* If a writer is parked, the drain may have opened room —
         * resume it. */
        if (p->parked_writer_rcvid != 0 && ring_space(p) > 0) {
            unsigned moved = ring_fill(p, p->parked_writer_buf,
                                        p->parked_writer_left);
            p->parked_writer_buf  += moved;
            p->parked_writer_left -= moved;
            if (p->parked_writer_left == 0) {
                /* Writer fully drained — reply to it. */
                MsgReply(p->parked_writer_rcvid, 0, 0, 0);
                p->parked_writer_rcvid = 0;
            }
        }
        return (long)got;
    }

    if (p->writer_count == 0) {
        /* No writers attached → EOF.  Return 0 bytes, caller sees
         * end-of-pipe. */
        return 0;
    }

    /* No data, writers still around → park the caller. */
    if (p->parked_reader_rcvid != 0) {
        /* v0.7 limit — should be impossible with single-reader use. */
        return -EAGAIN;
    }
    p->parked_reader_rcvid = rcvid;
    p->parked_reader_want  = want;
    return -1;   /* sentinel: "park, no reply" */
}

static long do_write(pipe_t *p, const unsigned char *src, unsigned want,
                     int rcvid)
{
    if (!p->in_use) return -EBADF;
    if (p->reader_count == 0) {
        /* SIGPIPE territory — POSIX says write returns EPIPE. */
        return -EPIPE;
    }

    /* Reader parked? deliver directly. */
    if (p->parked_reader_rcvid != 0) {
        unsigned give = want;
        if (give > p->parked_reader_want) give = p->parked_reader_want;
        if (give > 928u) give = 928u;
        unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
        for (unsigned i = 0; i < give; ++i) dst[i] = src[i];
        /* Reply MR0 = bytes delivered. */
        seL4_Word reply_word = (seL4_Word)give;
        MsgReply(p->parked_reader_rcvid, 0, &reply_word, sizeof reply_word);
        p->parked_reader_rcvid = 0;
        if (give == want) return (long)want;
        /* Leftover after delivery: fall through to fill ring. */
        src  += give;
        want -= give;
        if (want == 0) return (long)give;
    }

    unsigned put = ring_fill(p, src, want);
    if (put == want) return (long)put;

    /* Some bytes still pending: park writer for resumption on next drain. */
    if (p->parked_writer_rcvid != 0) return (long)put;  /* second writer racing */
    p->parked_writer_rcvid  = rcvid;
    p->parked_writer_buf    = src + put;
    p->parked_writer_left   = want - put;
    return -1;   /* park, partial-progress recorded */
}

/* ----------- main loop ----------- */

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    printf("[pipe] alive, pid=%d\n", (int)qsoe_self_pid);
    fflush(stdout);

    int chid = ChannelCreate(0);
    if (chid < 0) {
        printf("[pipe] FATAL: ChannelCreate failed (errno=%d)\n", qsoe_errno);
        return 1;
    }

    /* Announce /dev/pipe to the path manager.  Other resmgrs use
     * tm_pathmgr_register; libc users call us via open("/dev/pipe"). */
    if (qsoe_pathmgr_register(PIPE_MANAGER_PATH, chid) != 0) {
        printf("[pipe] FATAL: pathmgr register failed (errno=%d)\n",
               qsoe_errno);
        return 1;
    }
    printf("[pipe] registered at %s on chid=%d\n", PIPE_MANAGER_PATH, chid);
    fflush(stdout);

    /* QNX-style daemon: detach from parent so init's waitpid succeeds. */
    procmgr_detach(0);

    /* Main dispatch loop.  Every call is QNX-shape:
     *   rcvid = MsgReceive(chid, ...)
     *   inspect, fulfil or park
     *   MsgReply(rcvid, ...) or save for later
     */
    for (;;) {
        struct _msg_info info;
        seL4_Word msg[4] = { 0, 0, 0, 0 };
        int rcvid = MsgReceive(chid, msg, sizeof msg, &info);
        if (rcvid < 0) {
            /* Transient failure — keep the loop alive. */
            continue;
        }

        seL4_Word badge = (seL4_Word)info.scoid;
        unsigned uid = PIPE_ID_FROM_BADGE(badge);
        unsigned dir = PIPE_DIR_FROM_BADGE(badge);

        /* Label routes to IO_READ / IO_WRITE / CLOSE per the wire
         * protocol shared with the rest of QSOE. */
        unsigned label = (unsigned)(msg[0] >> 12);
        unsigned arg0  = (unsigned) msg[1];

        pipe_t *p = find_or_alloc_pipe(uid);
        if (!p) {
            /* Pool exhausted — back-pressure the caller. */
            MsgReply(rcvid, EMFILE, 0, 0);
            continue;
        }

        switch (label) {
        case TM_REQ_IO_READ: {
            if (dir != PIPE_DIR_READ) {
                MsgReply(rcvid, EBADF, 0, 0);
                break;
            }
            long n = do_read(p, arg0, rcvid);
            if (n == -1) {
                /* parked — no reply now */
                break;
            }
            if (n < 0) {
                MsgReply(rcvid, (int)(-n), 0, 0);
                break;
            }
            seL4_Word out = (seL4_Word)n;
            MsgReply(rcvid, 0, &out, sizeof out);
            break;
        }
        case TM_REQ_IO_WRITE: {
            if (dir != PIPE_DIR_WRITE) {
                MsgReply(rcvid, EBADF, 0, 0);
                break;
            }
            const unsigned char *src =
                (const unsigned char *)&qsoe_ipcbuf->msg[4];
            long n = do_write(p, src, arg0, rcvid);
            if (n == -1) {
                /* partial-park: writer's remaining bytes will be drained
                 * when readers consume more.  No reply now. */
                break;
            }
            if (n < 0) {
                MsgReply(rcvid, (int)(-n), 0, 0);
                break;
            }
            seL4_Word out = (seL4_Word)n;
            MsgReply(rcvid, 0, &out, sizeof out);
            break;
        }
        case TM_REQ_CLOSE: {
            if (p->in_use) {
                if (dir == PIPE_DIR_READ  && p->reader_count > 0) p->reader_count--;
                if (dir == PIPE_DIR_WRITE && p->writer_count > 0) p->writer_count--;
                /* Writer-side close with a parked reader → wake it
                 * with 0 (EOF). */
                if (p->writer_count == 0 && p->parked_reader_rcvid != 0) {
                    seL4_Word zero = 0;
                    MsgReply(p->parked_reader_rcvid, 0, &zero, sizeof zero);
                    p->parked_reader_rcvid = 0;
                }
                /* Reader-side close with a parked writer → wake it
                 * with EPIPE. */
                if (p->reader_count == 0 && p->parked_writer_rcvid != 0) {
                    MsgReply(p->parked_writer_rcvid, EPIPE, 0, 0);
                    p->parked_writer_rcvid = 0;
                }
                if (p->reader_count == 0 && p->writer_count == 0) {
                    p->in_use = 0;
                    p->head = p->tail = 0;
                }
            }
            MsgReply(rcvid, 0, 0, 0);
            break;
        }
        default:
            /* Unknown label — return ENOSYS so the client can fail clean. */
            MsgReply(rcvid, ENOSYS, 0, 0);
            break;
        }
    }
    return 0;
}
