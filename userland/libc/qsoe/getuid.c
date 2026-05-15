/*
 * getuid.c — POSIX getuid().
 *
 * Real UID (the cred a user identifies as).  Fetched from taskman's
 * per-process cred via TM_REQ_PROC_SELF_INFO.  In v0.7 every process
 * inherits root from taskman (no multi-user yet); once setuid() lands
 * this returns the real-uid that propagated through fork/spawn or was
 * set by privileged code.
 */

#include <unistd.h>
#include <qsoe-system.h>

uid_t getuid(void)
{
    struct _cred_info cred;
    if (qsoe_proc_self_info(0, 0, &cred) != 0) return 0;
    return cred.ruid;
}
