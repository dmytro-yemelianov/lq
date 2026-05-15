/*
 * proc/pulse.c — pulse send / fetch.  QNX-style async fixed-size
 * messages queued at a channel; receiver wakes via the channel's
 * bound Notification and pops via TM_REQ_PULSE_FETCH.
 *
 * Split out of v0.6.4's server.c.
 */

#include "proc.h"
#include "../qsoe_invoke.h"

/* connect.c-owned static helper — we only need to find a connection
 * by (sender_pid, slot) on the send path.  Re-implemented inline
 * here against the connections array would couple modules; instead
 * we route through the connections-lookup that connect.c already
 * exposes for cpiofs, then resolve the channel via tm_channels_array. */

int tm_pulse_send(pid_t sender_pid, seL4_CPtr connection_slot,
                  int priority, int code, int value)
{
    seL4_Word badge = 0;
    if (tm_connection_badge_by_slot(sender_pid, connection_slot, &badge) != 0) {
        return -EBADF;
    }
    pid_t srv_pid; int srv_chid;
    if (tm_channel_by_badge(badge, &srv_pid, &srv_chid) != 0) return -EBADF;

    int channel_idx = tm_channel_index(srv_pid, srv_chid);
    if (channel_idx < 0) return -EBADF;
    tm_channel_t *c = &tm_channels_array()[channel_idx];
    if (!c->in_use) return -EBADF;

    if (c->pulse_count >= TM_PULSE_QUEUE_LEN) return -EAGAIN;

    int slot = c->pulse_tail;
    c->pulse_queue[slot].sender_pid = sender_pid;
    c->pulse_queue[slot].priority   = priority;
    c->pulse_queue[slot].code       = code;
    c->pulse_queue[slot].value      = value;

    c->pulse_tail = (slot + 1) % TM_PULSE_QUEUE_LEN;
    c->pulse_count++;

    /* Wake the receiver via the channel's bound Notification.  If
     * none is set up (taskman's primary EP, or a channel whose
     * owner already had a binding when ChannelCreate ran), the
     * pulse stays queued for the next MsgReceive's fetch path. */
    if (c->ntfn_sig) {
        qsoe_sys_signal(c->ntfn_sig);
    }
    return 0;
}

int tm_pulse_fetch(pid_t receiver_pid, seL4_CPtr recv_slot,
                   tm_pulse_t *out_pulse, int *out_scoid)
{
    tm_channel_t *gchannels = tm_channels_array();
    tm_channel_t *c = 0;
    int idx = -1;
    for (int i = 0; i < TM_MAX_CHANNELS; ++i) {
        if (gchannels[i].in_use &&
            gchannels[i].owner_pid  == receiver_pid &&
            gchannels[i].owner_recv == recv_slot) {
            c = &gchannels[i];
            idx = i;
            break;
        }
    }
    if (!c) return -EBADF;
    if (c->pulse_count == 0) return -ENOENT;

    int slot = c->pulse_head;
    *out_pulse = c->pulse_queue[slot];
    c->pulse_head = (slot + 1) % TM_PULSE_QUEUE_LEN;
    c->pulse_count--;

    /* scoid: server's view of the sender's connection.  Look it up
     * via the connections public table by walking until we find the
     * sender's connection to this channel.  No public iterator yet
     * — drop the field on miss; tm_pulse_fetch consumers tolerate
     * scoid == 0 (the v0.6.4 behaviour). */
    *out_scoid = 0;
    (void)idx;

    /* If more pulses remain, re-arm the bound Notification so the
     * receiver's NEXT MsgReceive wakes immediately rather than
     * blocking on the endpoint Recv. */
    if (c->pulse_count > 0 && c->ntfn_sig) {
        qsoe_sys_signal(c->ntfn_sig);
    }
    return 0;
}
