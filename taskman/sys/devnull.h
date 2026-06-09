/* devnull.h — handlers for /dev/null traffic.  See devnull.c. */
#ifndef QSOE_TASKMAN_DEVNULL_H
#define QSOE_TASKMAN_DEVNULL_H

#include <sys/qsoe.h>   /* tm_stat_t */

unsigned tm_devnull_write(unsigned nbytes);
int      tm_devnull_read (unsigned want, unsigned *out_got);
int      tm_devnull_stat (tm_stat_t *out);

#endif
