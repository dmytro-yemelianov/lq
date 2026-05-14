/*
 * console.h — handlers for /dev/console traffic. See console.c.
 */
#ifndef QSOE_TASKMAN_CONSOLE_H
#define QSOE_TASKMAN_CONSOLE_H

unsigned tm_console_write(unsigned nbytes);
int      tm_console_read (unsigned want, unsigned *out_got);

#endif
