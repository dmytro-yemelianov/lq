/*
 * getppid.c — POSIX getppid().
 *
 * Asks taskman for the parent pid via TM_REQ_PROC_SELF_INFO.  taskman
 * tracks parent_pid in tm_process_t (set by tm_process_set_parent
 * right after spawn).  procmgr_detach reparents to pid 1 (taskman);
 * after that getppid() returns 1.
 */

#include <unistd.h>
#include <qsoe-system.h>

pid_t getppid(void)
{
    pid_t ppid = 0;
    if (qsoe_proc_self_info(0, &ppid, 0) != 0) return 0;
    return ppid;
}
