/*
 * ring.h — fixed-size single-producer / single-consumer byte ring.
 *
 * Used by devc-ser8250 to bridge between the IRQ thread (producer
 * for RX, drains the UART FIFO) and the main dispatch thread
 * (consumer for RX, replies to clients' read calls). The TX path
 * is polled in v0.6.1 so no ring is needed there.
 *
 * 256 bytes per ring is more than enough — the UART's hardware
 * FIFO is 16 bytes, and the main thread drains the software ring
 * almost as fast as the IRQ thread fills it.
 *
 * Sync is via a qsoe_spinlock_t. We're on SMP so we can't elide it
 * even though usage is "mostly single-producer single-consumer."
 */
#ifndef QSOE_DEVC_SER8250_RING_H
#define QSOE_DEVC_SER8250_RING_H

#include <qsoe/tls.h>  /* qsoe_spinlock_t */

#define SER_RING_SIZE 256

struct ser_ring {
    qsoe_spinlock_t lock;
    unsigned        head;        /* read index */
    unsigned        tail;        /* write index */
    unsigned        count;
    unsigned char   buf[SER_RING_SIZE];
};

void     ser_ring_init(struct ser_ring *r);
int      ser_ring_push(struct ser_ring *r, unsigned char b);  /* 0 ok, -1 full */
int      ser_ring_pop (struct ser_ring *r, unsigned char *b); /* 0 ok, -1 empty */
unsigned ser_ring_drain(struct ser_ring *r, unsigned char *dst, unsigned max);

#endif
