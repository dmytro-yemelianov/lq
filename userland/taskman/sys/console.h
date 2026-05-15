/*
 * console.h — handlers for /dev/console traffic. See console.c.
 */
#ifndef QSOE_TASKMAN_CONSOLE_H
#define QSOE_TASKMAN_CONSOLE_H

#include "../path/path.h"   /* tm_stat_t */

unsigned tm_console_write(unsigned nbytes);
int      tm_console_read (unsigned want, unsigned *out_got);
int      tm_console_stat (tm_stat_t *out);

#endif
