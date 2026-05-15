/*
 * getgid.c — POSIX getgid().
 *
 * Real GID from taskman's per-process cred via TM_REQ_PROC_SELF_INFO.
 */

#include <unistd.h>
#include <qsoe-system.h>

gid_t getgid(void)
{
    struct _cred_info cred;
    if (qsoe_proc_self_info(0, 0, &cred) != 0) return 0;
    return cred.rgid;
}
