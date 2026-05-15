/*
 * opendir.c — POSIX opendir().
 *
 * Allocate a DIR (musl's `struct __dirstream`), open the path
 * through our cpiofs-aware open(), stash the fd in the DIR.
 * readdir() and closedir() (the latter still upstream-musl) take
 * it from there.
 *
 * The DIR struct layout must match musl's bits/__dirent.h, so the
 * shape lives there — we just zero-initialise our extras (tell,
 * buf_pos, buf_end, lock, buf).
 */

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "__dirent.h"   /* musl struct __dirstream */

DIR *opendir(const char *name)
{
    int fd = open(name, O_RDONLY);
    if (fd < 0) return 0;

    DIR *d = malloc(sizeof *d);
    if (!d) { close(fd); return 0; }
    /* Zero everything; we only use fd ourselves but seekdir /
     * telldir / rewinddir touch the other fields. */
    unsigned char *p = (unsigned char *)d;
    for (unsigned i = 0; i < sizeof *d; ++i) p[i] = 0;
    d->fd = fd;
    return d;
}
