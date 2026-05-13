/*
 * <qsoe/wire.h> — internal wire protocol between libqsoe and taskman.
 *
 * This header is shared between libqsoe (request side) and taskman
 * (handler side) so the message-label numbers and MR layout stay in
 * sync. It is NOT part of the public API surface — application code
 * should never include it.
 *
 * See Design doc §4.4 "Wire protocol".
 */
#ifndef QSOE_WIRE_H
#define QSOE_WIRE_H

enum {
    TM_REQ_CHANNEL_CREATE  = 0x01,
    TM_REQ_CHANNEL_DESTROY = 0x02,
    TM_REQ_CONNECT_ATTACH  = 0x03,
    TM_REQ_CONNECT_DETACH  = 0x04,
};

/* Reply label conventions: 0 on success, negative QNX errno on failure. */

#endif /* QSOE_WIRE_H */
