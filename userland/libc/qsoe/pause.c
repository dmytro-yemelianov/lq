/*
 * pause.c — POSIX pause().
 *
 * Blocks the calling thread until a signal is delivered (POSIX:
 * pause shall return -1 with errno=EINTR after a signal-catching
 * function returns).
 *
 * Implementation: create a channel on first call (cache it for the
 * life of the process), then MsgReceive on it.  In v0.7 nothing
 * yet wakes this channel — the signal thread will MsgSendPulse on
 * it once libqsoe's signal-delivery-to-main-thread path is fully
 * wired — so for now pause() blocks until the process is killed.
 * That matches POSIX's semantic ("blocks until interrupted"); the
 * v0.7 limitation is that no internal source of interrupts exists
 * yet.
 *
 * Caching the channel avoids resource leaks for repeated pause()
 * calls (qsh's j_waitj is the heaviest user).
 */

#include <unistd.h>
#include <qsoe/qrv.h>
#include <sel4_types.h>   /* seL4_Word */

static int s_pause_chid = -1;

int pause(void)
{
    if (s_pause_chid < 0) {
        s_pause_chid = ChannelCreate(0);
        if (s_pause_chid < 0) return -1;
    }
    seL4_Word msg[4] = { 0, 0, 0, 0 };
    struct _msg_info info;
    int rcvid = MsgReceive(s_pause_chid, msg, sizeof msg, &info);
    (void)rcvid;
    /* POSIX: return -1 with errno=EINTR after a signal returns. */
    qsoe_errno = EINTR;
    return -1;
}
