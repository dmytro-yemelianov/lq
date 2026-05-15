/*
 * geteuid.c — POSIX geteuid().
 *
 * Effective UID — the cred the kernel checks on access calls.
 * Fetched from taskman's per-process cred via TM_REQ_PROC_SELF_INFO.
 */

#include <unistd.h>
#include <qsoe/qrv.h>

uid_t geteuid(void)
{
    struct _cred_info cred;
    if (qsoe_proc_self_info(0, 0, &cred) != 0) return 0;
    return cred.euid;
}
