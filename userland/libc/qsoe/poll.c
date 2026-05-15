/*
 * poll.c — v0.7 STUB (real implementation lands in v0.8).
 *
 * Real poll() needs per-fd readiness notification (pulse-based, one
 * Notification per pollfd) plus a timer for the timeout — see the
 * memory record `project_poll_v08.md` for the v0.8 design sketch.
 *
 * Until v0.8 every call returns 0 with every revents cleared, which
 * matches POSIX's "timeout elapsed, no events" path.  Callers that
 * use poll() for read-readiness fall through to a blocking read.
 * Marked _Pragma so this stub is loud at compile time — future me
 * sees the v0.8 TODO if the warning vanishes (stub got removed).
 */

#include <poll.h>

_Pragma("message \"v0.7 STUB: poll() — real impl deferred to v0.8\"")

int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    (void)timeout;
    for (nfds_t i = 0; i < nfds; ++i) {
        if (fds) fds[i].revents = 0;
    }
    return 0;
}
