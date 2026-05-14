/*
 * ring.c — see ring.h.
 */
#include "ring.h"

void ser_ring_init(struct ser_ring *r)
{
    r->lock = 0;
    r->head = r->tail = r->count = 0;
}

int ser_ring_push(struct ser_ring *r, unsigned char b)
{
    int rc = -1;
    qsoe_spin_lock(&r->lock);
    if (r->count < SER_RING_SIZE) {
        r->buf[r->tail] = b;
        r->tail = (r->tail + 1) % SER_RING_SIZE;
        r->count++;
        rc = 0;
    }
    qsoe_spin_unlock(&r->lock);
    return rc;
}

int ser_ring_pop(struct ser_ring *r, unsigned char *b)
{
    int rc = -1;
    qsoe_spin_lock(&r->lock);
    if (r->count > 0) {
        *b = r->buf[r->head];
        r->head = (r->head + 1) % SER_RING_SIZE;
        r->count--;
        rc = 0;
    }
    qsoe_spin_unlock(&r->lock);
    return rc;
}

unsigned ser_ring_drain(struct ser_ring *r, unsigned char *dst, unsigned max)
{
    unsigned n = 0;
    qsoe_spin_lock(&r->lock);
    while (n < max && r->count > 0) {
        dst[n++] = r->buf[r->head];
        r->head = (r->head + 1) % SER_RING_SIZE;
        r->count--;
    }
    qsoe_spin_unlock(&r->lock);
    return n;
}
