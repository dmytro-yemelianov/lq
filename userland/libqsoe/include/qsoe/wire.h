/*
 * <qsoe/wire.h> — internal wire protocol between libqsoe and taskman.
 *
 * Shared between libqsoe (request side) and taskman (handler side) so
 * message-label numbers and MR layout stay in sync. Not part of the
 * public API surface — application code should never include this.
 *
 * See Design doc §4.4 "Wire protocol".
 */
#ifndef QSOE_WIRE_H
#define QSOE_WIRE_H

enum {
    TM_REQ_CHANNEL_CREATE       = 0x01,
    TM_REQ_CHANNEL_DESTROY      = 0x02,
    TM_REQ_CONNECT_ATTACH       = 0x03,
    TM_REQ_CONNECT_DETACH       = 0x04,
    /* v0.3.3 */
    TM_REQ_CONNECT_SERVER_INFO  = 0x05,
    TM_REQ_CONNECT_CLIENT_INFO  = 0x06,
    TM_REQ_CONNECT_FLAGS        = 0x07,
    /* v0.4 */
    TM_REQ_THREAD_ALLOC         = 0x08,
    TM_REQ_THREAD_DESTROY       = 0x09,
    /* v0.4.1 */
    TM_REQ_PROCESS_CREATE       = 0x0a,
    TM_REQ_PROCESS_TERMINATE    = 0x0b,
    TM_REQ_DEBUG_SLOT_COUNT     = 0x0c,  /* cap-leak smoke test */
    /* v0.4.2 */
    TM_REQ_PULSE_SEND           = 0x0d,
    TM_REQ_PULSE_FETCH          = 0x0e,
    /* Demo / smoke-test label used by tester to trigger taskman's
     * server-side ConnectClientInfo print. Echoes MR0+1 like the
     * default v0.3.2 echo, but additionally fills MR1 with the
     * client pid that taskman saw via ConnectClientInfo. */
    TM_REQ_PING_CLIENTINFO      = 0x10,
    /* v0.5.0 POSIX I/O surface. The 0x20+ range disambiguates the
     * libc-facing wire from the taskman-internal management wire
     * above. open() consults the path manager; write/read/close
     * operate on a connection (badge identifies it). */
    TM_REQ_OPEN                 = 0x20,
    TM_REQ_CLOSE                = 0x21,
    TM_REQ_IO_WRITE             = 0x22,
    TM_REQ_IO_READ              = 0x23,
};

/* Reply label conventions: 0 on success, positive QNX errno on failure.
 * (Taskman uses positive values; libqsoe negates only when it stores
 * them in qsoe_errno, since the public surface uses positive errnos.)
 */

#endif /* QSOE_WIRE_H */
