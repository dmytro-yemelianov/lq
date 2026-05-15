/*
 * getpgrp.c — POSIX getpgrp().
 *
 * Process group of the caller.  QSOE has no session/group machinery
 * yet (no setsid / setpgid / job control), so every process is its
 * own group — pgrp == pid — matching the QRV convention and POSIX's
 * default-leader behaviour for a process that never joined a group.
 *
 * When sessions land, this reads from a per-process pgrp_id taskman
 * tracks alongside parent_pid; until then it simply calls getpid().
 */

#include <unistd.h>

pid_t getpgrp(void)
{
    return getpid();
}
