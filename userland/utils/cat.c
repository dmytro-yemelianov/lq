/*
 * cat — concatenate files to stdout.
 *
 * Standard POSIX cat.  With no file arguments, reads from stdin.
 * A single "-" argument means stdin.  Binary-safe: uses a fixed
 * buffer and copies byte-for-byte without any text translation.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define BUFSZ   4096

/* Copy fd → stdout until EOF or error.  Returns 0 on success, -1 on error. */
static int copy_fd(int fd, const char *name)
{
    static char buf[BUFSZ];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(1, buf + off, (size_t)(n - off));
            if (w < 0) {
                fprintf(stderr, "cat: write: %s\n", strerror(errno));
                return -1;
            }
            off += w;
        }
    }
    if (n < 0) {
        fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0;

    if (argc < 2) {
        return copy_fd(0, "stdin") != 0;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            if (copy_fd(0, "stdin") != 0)
                rc = 1;
            continue;
        }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (copy_fd(fd, argv[i]) != 0)
            rc = 1;
        close(fd);
    }
    return rc;
}
