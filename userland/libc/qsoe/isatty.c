/*
 * isatty.c — POSIX isatty().
 *
 * Calls fstat() on the fd and checks if the resmgr stat'd it as a
 * character device.  QSOE v0.7's only character device is
 * /dev/console (handed back as S_IFCHR by tm_console_stat); cpiofs
 * fds come back as S_IFREG and answer 0 correctly.  Future tty
 * resmgrs that emulate POSIX-shape pty semantics just need to also
 * report S_IFCHR.
 */

#include <unistd.h>
#include <sys/stat.h>

int isatty(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;
    return S_ISCHR(st.st_mode) ? 1 : 0;
}
