/*
 * strerror.c — error-number → message.
 *
 * Upstream musl's src/errno/strerror.c is locale-aware via LCTRANS;
 * we excluded that file with the locale subsystem.  QSOE returns
 * plain English messages, no translation, no per-thread buffer.
 *
 * The table is keyed by the QNX/QSOE errno values from
 * <qsoe/qrv.h> (matching musl's bits/errno.h for now).  Add entries
 * as more callers turn up undefined refs that point here.
 */

static const char *const messages[] = {
    [0]    = "No error",
    [1]    = "Operation not permitted",
    [2]    = "No such file or directory",
    [3]    = "No such process",
    [4]    = "Interrupted system call",
    [5]    = "I/O error",
    [6]    = "No such device or address",
    [7]    = "Argument list too long",
    [8]    = "Exec format error",
    [9]    = "Bad file descriptor",
    [10]   = "No child processes",
    [11]   = "Resource temporarily unavailable",
    [12]   = "Out of memory",
    [13]   = "Permission denied",
    [14]   = "Bad address",
    [16]   = "Device or resource busy",
    [17]   = "File exists",
    [19]   = "No such device",
    [20]   = "Not a directory",
    [21]   = "Is a directory",
    [22]   = "Invalid argument",
    [23]   = "Too many open files in system",
    [24]   = "Too many open files",
    [25]   = "Not a tty",
    [27]   = "File too large",
    [28]   = "No space left on device",
    [29]   = "Illegal seek",
    [30]   = "Read-only file system",
    [31]   = "Too many links",
    [32]   = "Broken pipe",
};

static const unsigned messages_count = sizeof messages / sizeof messages[0];

char *strerror(int e);
char *strerror(int e)
{
    const char *s;
    if (e < 0 || (unsigned)e >= messages_count || !messages[e])
        s = "Unknown error";
    else
        s = messages[e];
    return (char *)s;
}
