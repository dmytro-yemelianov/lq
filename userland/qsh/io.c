/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * I/O, file descriptors, pipes, terminal init, debug log.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

#define NSHF_IOB 3
struct shf shf_iob[NSHF_IOB];

/*
 * Initialise tty_fd. Used for tracking the size of the terminal,
 * saving/resetting tty modes upon foreground job completion, and
 * for setting up the tty process group. Return values:
 *  0 = got controlling tty
 *  1 = got terminal but no controlling tty
 *  2 = cannot find a terminal
 *  3 = cannot dup fd
 *  4 = cannot make fd close-on-exec
 * An existing tty_fd is cached if no "better" one could be found,
 * i.e. if tty_devtty was already set or the new would not set it.
 */
int
tty_init_fd(void)
{
    int fd, rv, eno = 0;
    bool do_close = false, is_devtty = true;

    if (tty_devtty) {
        /* already got a tty which is /dev/tty */
        return (0);
    }

    if ((fd = open("/dev/tty", O_RDWR, 0)) >= 0) {
        do_close = true;
        goto got_fd;
    }
    eno = errno;

    if (tty_fd >= 0) {
        /* already got a non-devtty one */
        rv = 1;
        goto out;
    }
    is_devtty = false;

    if (isatty((fd = 0)) || isatty((fd = 2)))
        goto got_fd;
    /* cannot find one */
    rv = 2;
    /* assert: do_close == false */
    goto out;

got_fd:
    if ((rv = fcntl(fd, F_DUPFD, FDBASE)) < 0) {
        eno = errno;
        rv = 3;
        goto out;
    }
    if (fcntl(rv, F_SETFD, FD_CLOEXEC) < 0) {
        eno = errno;
        close(rv);
        rv = 4;
        goto out;
    }
    tty_fd = rv;
    tty_devtty = is_devtty;
    rv = eno = 0;
out:
    if (do_close)
        close(fd);
    errno = eno;
    return (rv);
}

/* printf to shl_out (stderr) with flush */
void
shellf(const char *fmt, ...)
{
    va_list va;

    va_start(va, fmt);
    shf_vfprintf(shl_out, fmt, va);
    va_end(va);
    shf_flush(shl_out);
}

/* printf to shl_stdout (stdout) */
void
shprintf(const char *fmt, ...)
{
    va_list va;

    if (!shl_stdout_ok)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_ONEMSG | KWF_NOERRNO, "shl_stdout not valid");
    va_start(va, fmt);
    shf_vfprintf(shl_stdout, fmt, va);
    va_end(va);
}

/* test if we can seek backwards fd (returns 0 or SHF_UNBUF) */
int
can_seek(int fd)
{
    struct stat statb;

    return (fstat(fd, &statb) == 0 && !S_ISREG(statb.st_mode) ? SHF_UNBUF : 0);
}

/* pre-initio() */
void
initio(void)
{
    /* force buffer allocation */
    shf_fdopen(1, SHF_WR, shl_stdout);
    shf_fdopen(2, SHF_WR, shl_out);
    shf_fdopen(2, SHF_WR, shl_xtrace);
    initio_done = true;
}

/* A dup2() with error checking */
int
qsh_dup2(int ofd, int nfd, bool errok)
{
    int rv;

    if (((rv = dup2(ofd, nfd)) < 0) && !errok && (errno != EBADF))
        kerrf0(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE, Ttoo_many_files, ofd, nfd);

    return (rv);
}

/*
 * Move fd from user space (0 <= fd < FDBASE) to shell space (fd >= FDBASE)
 * set moved fd's close-on-exec flag (see sh.h for FDBASE).
 */
int
savefd(int fd)
{
    int nfd = fd;

    errno = 0;
    if (fd < FDBASE && (nfd = fcntl(fd, F_DUPFD, FDBASE)) < 0 && (errno == EBADF || errno == EPERM))
        return (-1);
    if (nfd < FDBASE || nfd > (int)(kui)FDMAXNUM)
        kerrf0(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE, Ttoo_many_files, fd, nfd);
    if (fcntl(nfd, F_SETFD, FD_CLOEXEC) == -1)
        kwarnf0(KWF_INTERNAL | KWF_WARNING, Tcloexec_failed, "set", nfd);
    return (nfd);
}

void
restfd(int fd, int ofd)
{
    if (fd == 2)
        shf_flush(&shf_iob[/* fd */ 2]);
    if (ofd < 0)
        /* original fd closed */
        close(fd);
    else if (fd != ofd) {
        /*XXX: what to do if this dup fails? */
        qsh_dup2(ofd, fd, true);
        close(ofd);
    }
}

void
openpipe(int *pv)
{
    int lpv[2];

    if (pipe(lpv) < 0)
        kerrf(KWF_ERR(1) | KWF_PREFIX | KWF_FILELINE | KWF_ONEMSG, "pipe");
    pv[0] = savefd(lpv[0]);
    if (pv[0] != lpv[0])
        close(lpv[0]);
    pv[1] = savefd(lpv[1]);
    if (pv[1] != lpv[1])
        close(lpv[1]);
}

void
closepipe(int *pv)
{
    close(pv[0]);
    close(pv[1]);
}

/*
 * Called by iosetup() (deals with 2>&4, etc.), c_read, c_print to turn
 * a string (the X in 2>&X, read -uX, print -uX) into a file descriptor.
 */
int
check_fd(const char *name, int mode, const char **emsgp)
{
    int fd, fl;

    if (!name[0] || name[1])
        goto illegal_fd_name;
    if (name[0] == 'p')
        return (coproc_getfd(mode, emsgp));
    if (!ctype(name[0], C_DIGIT)) {
    illegal_fd_name:
        if (emsgp)
            *emsgp = "illegal file descriptor name";
        errno = EINVAL;
        return (-1);
    }

    if ((fl = fcntl((fd = qsh_numdig(name[0])), F_GETFL, 0)) < 0) {
        if (emsgp)
            *emsgp = "bad file descriptor";
        return (-1);
    }
    fl &= O_ACCMODE;
    /*
     * X_OK is a kludge to disable this check for dups (x<&1):
     * historical shells never did this check (XXX don't know what
     * POSIX has to say).
     */
    if (!(mode & X_OK) && fl != O_RDWR &&
        (((mode & R_OK) && fl != O_RDONLY) || ((mode & W_OK) && fl != O_WRONLY))) {
        if (emsgp)
            *emsgp = (fl == O_WRONLY) ? "fd not open for reading" : "fd not open for writing";
#ifdef ENXIO
        errno = ENXIO;
#else
        errno = EBADF;
#endif
        return (-1);
    }
    return (fd);
}
