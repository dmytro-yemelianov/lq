/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Co-process state and lifecycle.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/* Called once from main */
void
coproc_init(void)
{
    coproc.read = coproc.readw = coproc.write = -1;
    coproc.njobs = 0;
    coproc.id = 0;
}

/* Called by c_read() when eof is read - close fd if it is the co-process fd */
void
coproc_read_close(int fd)
{
    if (coproc.read >= 0 && fd == coproc.read) {
        coproc_readw_close(fd);
        close(coproc.read);
        coproc.read = -1;
    }
}

/*
 * Called by c_read() and by iosetup() to close the other side of the
 * read pipe, so reads will actually terminate.
 */
void
coproc_readw_close(int fd)
{
    if (coproc.readw >= 0 && coproc.read >= 0 && fd == coproc.read) {
        close(coproc.readw);
        coproc.readw = -1;
    }
}

/*
 * Called by c_print when a write to a fd fails with EPIPE and by iosetup
 * when co-process input is dup'd
 */
void
coproc_write_close(int fd)
{
    if (coproc.write >= 0 && fd == coproc.write) {
        close(coproc.write);
        coproc.write = -1;
    }
}

/*
 * Called to check for existence of/value of the co-process file descriptor.
 * (Used by check_fd() and by c_read/c_print to deal with -p option).
 */
int
coproc_getfd(int mode, const char **emsgp)
{
    int fd = (mode & R_OK) ? coproc.read : coproc.write;

    if (fd >= 0)
        return (fd);
    if (emsgp)
        *emsgp = "no coprocess";
    errno = EBADF;
    return (-1);
}

/*
 * called to close file descriptors related to the coprocess (if any)
 * Should be called with SIGCHLD blocked.
 */
void
coproc_cleanup(int reuse)
{
    /* This to allow co-processes to share output pipe */
    if (!reuse || coproc.readw < 0 || coproc.read < 0) {
        if (coproc.read >= 0) {
            close(coproc.read);
            coproc.read = -1;
        }
        if (coproc.readw >= 0) {
            close(coproc.readw);
            coproc.readw = -1;
        }
    }
    if (coproc.write >= 0) {
        close(coproc.write);
        coproc.write = -1;
    }
}
