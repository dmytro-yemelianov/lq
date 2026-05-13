/*
 * <qsoe/slots.h> — well-known CSpace slot conventions, shared by libqsoe
 * and taskman. See Design doc §4.5 "CSpace conventions".
 */
#ifndef QSOE_SLOTS_H
#define QSOE_SLOTS_H

#define QSOE_CAP_NULL          0
#define QSOE_CAP_TASKMAN_EP    1   /* Send cap to taskman's primary endpoint */
#define QSOE_CAP_OWN_UNTYPED   2   /* This process's untyped budget (v0.4.1+) */
#define QSOE_CAP_WELL_KNOWN_END 16 /* slots [2..15] reserved; dynamics start at 16 */

/* By convention pid 1 is taskman itself. */
#define QSOE_PID_TASKMAN       1

#endif /* QSOE_SLOTS_H */
