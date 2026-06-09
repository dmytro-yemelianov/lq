/* devzero.h — handlers for /dev/zero traffic.  See devzero.c. */
#ifndef QSOE_TASKMAN_DEVZERO_H
#define QSOE_TASKMAN_DEVZERO_H

#include <sys/qsoe.h>   /* tm_stat_t */

unsigned tm_devzero_write(unsigned nbytes);
int      tm_devzero_read (unsigned want, unsigned *out_got);
int      tm_devzero_stat (tm_stat_t *out);

#endif
