/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/* flags to shf_emptybuf() */
#define EB_READSW 0x01 /* about to switch to reading */
#define EB_GROW 0x02   /* grow buffer if necessary (STRING+DYNAMIC) */

/*
 * Replacement stdio routines. Stdio is too flakey on too many machines
 * to be useful when you have multiple processes using the same underlying
 * file descriptors.
 */

static int shf_fillbuf(struct shf *);
static int shf_emptybuf(struct shf *, int);

/*
 * Open a file. First three args are for open(), last arg is flags for
 * this package. Returns NULL if file could not be opened, or if a dup
 * fails.
 */
struct shf *
shf_open(const char *name, int oflags, int mode, int sflags)
{
    struct shf *shf;
    ssize_t bsize =
        /* at most 512 */
        sflags & SHF_UNBUF ? (sflags & SHF_RD ? 1 : 0) : SHF_BSIZE;
    int fd, eno;

    /* Done before open so if alloca fails, fd won't be lost. */
    shf = alloc1(sizeof(struct shf), bsize, ATEMP);
    shf->areap = ATEMP;
    shf->buf = (unsigned char *)&shf[1];
    shf->bsize = bsize;
    shf->flags = SHF_ALLOCS;
    /* Rest filled in by reopen. */

    fd = binopen3(name, oflags, mode);
    if (fd < 0) {
        eno = errno;
        afree(shf, shf->areap);
        errno = eno;
        return (NULL);
    }
    if ((sflags & SHF_MAPHI) && fd < FDBASE) {
        int nfd;

        nfd = fcntl(fd, F_DUPFD, FDBASE);
        eno = errno;
        close(fd);
        if (nfd < 0) {
            afree(shf, shf->areap);
            errno = eno;
            return (NULL);
        }
        fd = nfd;
    }
    sflags &= ~SHF_ACCMODE;
    sflags |= (oflags & O_ACCMODE) == O_RDONLY
                  ? SHF_RD
                  : ((oflags & O_ACCMODE) == O_WRONLY ? SHF_WR : SHF_RDWR);

    return (shf_reopen(fd, sflags, shf));
}

/* helper function for shf_fdopen and shf_reopen */
/* pre-initio() *sflagsp=SHF_WR */
static void
shf_open_hlp(int fd, int *sflagsp, const char *where)
{
    int sflags = *sflagsp;

    /* use fcntl() to figure out correct read/write flags */
    if (sflags & SHF_GETFL) {
        int flags = fcntl(fd, F_GETFL, 0);

        if (flags < 0)
            /* will get an error on first read/write */
            sflags |= SHF_RDWR;
        else {
            switch (flags & O_ACCMODE) {
            case O_RDONLY:
                sflags |= SHF_RD;
                break;
            case O_WRONLY:
                sflags |= SHF_WR;
                break;
            case O_RDWR:
                sflags |= SHF_RDWR;
                break;
            }
        }
        *sflagsp = sflags;
    }

    if (!(sflags & (SHF_RD | SHF_WR)))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, where, sflags);
}

/* Set up the shf structure for a file descriptor. Doesn't fail. */
/* pre-initio() sflags=SHF_WR */
struct shf *
shf_fdopen(int fd, int sflags, struct shf *shf)
{
    ssize_t bsize =
        /* at most 512 */
        (sflags & SHF_UNBUF) ? ((sflags & SHF_RD) ? 1 : 0) : SHF_BSIZE;

    shf_open_hlp(fd, &sflags, "shf_fdopen");
    if (shf) {
        if (bsize) {
            shf->buf = alloc(bsize, ATEMP);
            sflags |= SHF_ALLOCB;
        } else
            shf->buf = NULL;
    } else {
        unsigned char *cp;

        cp = alloc1(sizeof(struct shf), bsize, ATEMP);
        shf = (void *)cp;
        shf->buf = cp + sizeof(struct shf);
        sflags |= SHF_ALLOCS;
    }
    shf->areap = ATEMP;
    shf->fd = fd;
    shf->rp = shf->wp = shf->buf;
    shf->rnleft = 0;
    shf->rbsize = bsize;
    shf->wnleft = 0; /* force call to shf_emptybuf() */
    shf->wbsize = sflags & SHF_UNBUF ? 0 : bsize;
    shf->flags = sflags;
    shf->errnosv = 0;
    shf->bsize = bsize;
    if ((sflags & SHF_CLEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        kwarnf0(KWF_INTERNAL | KWF_WARNING, Tcloexec_failed, "set", fd);
    return (shf);
}

/* Set up an existing shf (and buffer) to use the given fd */
struct shf *
shf_reopen(int fd, int sflags, struct shf *shf)
{
    ssize_t bsize =
        /* at most 512 */
        sflags & SHF_UNBUF ? (sflags & SHF_RD ? 1 : 0) : SHF_BSIZE;

    shf_open_hlp(fd, &sflags, "shf_reopen");
    if (!shf->buf || shf->bsize < bsize)
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_buf, "shf_reopen", (size_t)shf->buf,
               shf->bsize);

    /* assumes shf->buf and shf->bsize already set up */
    shf->fd = fd;
    shf->rp = shf->wp = shf->buf;
    shf->rnleft = 0;
    shf->rbsize = bsize;
    shf->wnleft = 0; /* force call to shf_emptybuf() */
    shf->wbsize = sflags & SHF_UNBUF ? 0 : bsize;
    shf->flags = (shf->flags & (SHF_ALLOCS | SHF_ALLOCB)) | sflags;
    shf->errnosv = 0;
    if ((sflags & SHF_CLEXEC) && fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
        kwarnf0(KWF_INTERNAL | KWF_WARNING, Tcloexec_failed, "set", fd);
    return (shf);
}

/*
 * Open a string for reading or writing. If reading, bsize is the number
 * of bytes that can be read. If writing, bsize is the maximum number of
 * bytes that can be written. If shf is not NULL, it is filled in and
 * returned, if it is NULL, shf is allocated. If writing and buf is NULL
 * and SHF_DYNAMIC is set, the buffer is allocated (if bsize > 0, it is
 * used for the initial size). Doesn't fail.
 * When writing, a byte is reserved for a trailing NUL - see shf_sclose().
 */
struct shf *
shf_sopen(char *buf, ssize_t bsize, int sflags, struct shf *shf)
{
    if (!((sflags & SHF_RD) ^ (sflags & SHF_WR)))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, "shf_sopen", sflags);

    if (!shf) {
        shf = alloc(sizeof(struct shf), ATEMP);
        sflags |= SHF_ALLOCS;
    }
    shf->areap = ATEMP;
    if (!buf && (sflags & SHF_WR) && (sflags & SHF_DYNAMIC)) {
        if (bsize <= 0)
            bsize = 64;
        sflags |= SHF_ALLOCB;
        buf = alloc(bsize, shf->areap);
    }
    shf->fd = -1;
    shf->buf = shf->rp = shf->wp = (unsigned char *)buf;
    shf->rnleft = bsize;
    shf->rbsize = bsize;
    shf->wnleft = bsize - 1; /* space for a '\0' */
    shf->wbsize = bsize;
    shf->flags = sflags | SHF_STRING;
    shf->errnosv = 0;
    shf->bsize = bsize;

    return (shf);
}

/* Open a string for dynamic writing, using already-allocated buffer */
struct shf *
shf_sreopen(char *buf, ssize_t bsize, Area *ap, struct shf *oshf)
{
    struct shf *shf;

    shf = shf_sopen(buf, bsize, SHF_WR | SHF_DYNAMIC, oshf);
    shf->areap = ap;
    shf->flags |= SHF_ALLOCB;
    return (shf);
}

/* Check whether the string can grow to take n bytes, close it up otherwise */
int
shf_scheck_grow(ssize_t n, struct shf *shf)
{
    if (!(shf->flags & SHF_WR))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, "shf_scheck", shf->flags);

    /* if n < 0 we lose in the macro already */

    /* nōn-string can always grow flushing */
    if (!(shf->flags & SHF_STRING))
        return (0);

    while (shf->wnleft < n)
        if (shf_emptybuf(shf, EB_GROW) == -1)
            break;

    if (shf->wnleft < n) {
        /* block subsequent writes as we truncate here */
        shf->wnleft = 0;
        return (1);
    }
    return (0);
}

/* Flush and close file descriptor, free the shf structure */
int
shf_close(struct shf *shf)
{
    int ret = 0;

    if (shf->fd >= 0) {
        ret = shf_flush(shf);
        if (close(shf->fd) < 0)
            ret = -1;
    }
    if (shf->flags & SHF_ALLOCS)
        afree(shf, shf->areap);
    else if (shf->flags & SHF_ALLOCB)
        afree(shf->buf, shf->areap);

    return (ret);
}

/* Flush and close file descriptor, don't free file structure */
int
shf_fdclose(struct shf *shf)
{
    int ret = 0;

    if (shf->fd >= 0) {
        ret = shf_flush(shf);
        if (close(shf->fd) < 0)
            ret = -1;
        shf->rnleft = 0;
        shf->rp = shf->buf;
        shf->wnleft = 0;
        shf->fd = -1;
    }

    return (ret);
}

/*
 * Close a string - if it was opened for writing, it is NUL terminated;
 * returns a pointer to the string and frees shf if it was allocated
 * (does not free string if it was allocated).
 */
char *
shf_sclose(struct shf *shf)
{
    unsigned char *s = shf->buf;

    /* NUL terminate */
    if (shf->flags & SHF_WR)
        *shf->wp = '\0';
    if (shf->flags & SHF_ALLOCS)
        afree(shf, shf->areap);
    return ((char *)s);
}

/*
 * Un-read what has been read but not examined, or write what has been
 * buffered. Returns 0 for success, -1 for (write) error.
 */
int
shf_flush(struct shf *shf)
{
    int rv = 0;

    if (shf->flags & SHF_STRING)
        rv = (shf->flags & SHF_WR) ? -1 : 0;
    else if (shf->fd < 0)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_TWOMSG | KWF_NOERRNO, "shf_flush", "no fd");
    else if (shf->flags & SHF_ERROR) {
        errno = shf->errnosv;
        rv = -1;
    } else if (shf->flags & SHF_READING) {
        shf->flags &= ~(SHF_EOF | SHF_READING);
        if (shf->rnleft > 0) {
            if (lseek(shf->fd, (off_t)-shf->rnleft, SEEK_CUR) == -1) {
                shf->flags |= SHF_ERROR;
                shf->errnosv = errno;
                rv = -1;
            }
            shf->rnleft = 0;
            shf->rp = shf->buf;
        }
    } else if (shf->flags & SHF_WRITING)
        rv = shf_emptybuf(shf, 0);

    return (rv);
}

/*
 * Write out any buffered data. If currently reading, flushes the read
 * buffer. Returns 0 for success, -1 for (write) error.
 */
static int
shf_emptybuf(struct shf *shf, int flags)
{
    int ret = 0;

    if (!(shf->flags & SHF_STRING) && shf->fd < 0)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_TWOMSG | KWF_NOERRNO, "shf_emptybuf", "no fd");

    if (shf->flags & SHF_ERROR) {
        errno = shf->errnosv;
        return (-1);
    }

    if (shf->flags & SHF_READING) {
        if (flags & EB_READSW)
            /* doesn't happen */
            return (0);
        ret = shf_flush(shf);
        shf->flags &= ~SHF_READING;
    }
    if (shf->flags & SHF_STRING) {
        size_t rp, wp;

        /*
         * Note that we assume SHF_ALLOCS is not set if
         * SHF_ALLOCB is set... (changing the shf pointer could
         * cause problems)
         */
        if (!(flags & EB_GROW) || !(shf->flags & SHF_DYNAMIC) || !(shf->flags & SHF_ALLOCB))
            return (-1);
        /* allocate more space for buffer */
        rp = shf->rp - shf->buf;
        wp = shf->wp - shf->buf;
        shf->buf = aresize2(shf->buf, 2, shf->wbsize, shf->areap);
        shf->rp = shf->buf + rp;
        shf->wp = shf->buf + wp;
        shf->rbsize += shf->wbsize;
        shf->wnleft += shf->wbsize;
        shf->wbsize <<= 1;
    } else {
        if (shf->flags & SHF_WRITING) {
            ssize_t n, ntowrite = shf->wp - shf->buf;
            unsigned char *buf = shf->buf;

            while (ntowrite > 0) {
                n = write(shf->fd, buf, ntowrite);
                if (n < 0) {
                    if (errno == EINTR && !(shf->flags & SHF_INTERRUPT))
                        continue;
                    shf->flags |= SHF_ERROR;
                    shf->errnosv = errno;
                    shf->wnleft = 0;
                    if (buf != shf->buf) {
                        /*
                         * allow a second flush
                         * to work
                         */
                        memmove(shf->buf, buf, ntowrite);
                        shf->wp = shf->buf + ntowrite;
                        /* restore errno for caller */
                        errno = shf->errnosv;
                    }
                    return (-1);
                }
                buf += n;
                ntowrite -= n;
            }
            if (flags & EB_READSW) {
                shf->wp = shf->buf;
                shf->wnleft = 0;
                shf->flags &= ~SHF_WRITING;
                return (0);
            }
        }
        shf->wp = shf->buf;
        shf->wnleft = shf->wbsize;
    }
    shf->flags |= SHF_WRITING;

    return (ret);
}

/* Fill up a read buffer. Returns -1 for a read error, 0 otherwise. */
static int
shf_fillbuf(struct shf *shf)
{
    ssize_t n;

    if (shf->flags & SHF_STRING)
        return (0);

    if (shf->fd < 0)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_TWOMSG | KWF_NOERRNO, "shf_fillbuf", "no fd");

    if (shf->flags & (SHF_EOF | SHF_ERROR)) {
        if (shf->flags & SHF_ERROR)
            errno = shf->errnosv;
        return (-1);
    }

    if ((shf->flags & SHF_WRITING) && shf_emptybuf(shf, EB_READSW) == -1)
        return (-1);

    shf->flags |= SHF_READING;

    shf->rp = shf->buf;
    while (/* CONSTCOND */ 1) {
        n = blocking_read(shf->fd, (char *)shf->buf, shf->rbsize);
        if (n < 0 && errno == EINTR && !(shf->flags & SHF_INTERRUPT))
            continue;
        break;
    }
    if (n < 0) {
        shf->flags |= SHF_ERROR;
        shf->errnosv = errno;
        shf->rnleft = 0;
        shf->rp = shf->buf;
        return (-1);
    }
    if ((shf->rnleft = n) == 0)
        shf->flags |= SHF_EOF;
    return (0);
}

/*
 * Read a buffer from shf. Returns the number of bytes read into buf, if
 * no bytes were read, returns 0 if end of file was seen, -1 if a read
 * error occurred.
 */
ssize_t
shf_read(char *buf, ssize_t bsize, struct shf *shf)
{
    ssize_t ncopy, orig_bsize = bsize;

    if (!(shf->flags & SHF_RD))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, Tshf_read, shf->flags);

    if (bsize <= 0)
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_buf, Tshf_read, (size_t)buf, bsize);

    while (bsize > 0) {
        if (shf->rnleft == 0 && (shf_fillbuf(shf) == -1 || shf->rnleft == 0))
            break;
        ncopy = shf->rnleft;
        if (ncopy > bsize)
            ncopy = bsize;
        memcpy(buf, shf->rp, ncopy);
        buf += ncopy;
        bsize -= ncopy;
        shf->rp += ncopy;
        shf->rnleft -= ncopy;
    }
    /* Note: fread(3S) returns 0 for errors - this doesn't */
    return (orig_bsize == bsize ? (shf_error(shf) ? -1 : 0) : orig_bsize - bsize);
}

/*
 * Read up to a newline or -1. The newline is put in buf; buf is always
 * NUL terminated. Returns NULL on read error or if nothing was read
 * before end of file, returns a pointer to the NUL byte in buf
 * otherwise.
 */
char *
shf_getse(char *buf, ssize_t bsize, struct shf *shf)
{
    unsigned char *end;
    ssize_t ncopy;
    char *orig_buf = buf;

    if (!(shf->flags & SHF_RD))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, "shf_getse", shf->flags);

    if (bsize <= 0)
        return (NULL);

    /* save room for NUL */
    --bsize;
    do {
        if (shf->rnleft == 0) {
            if (shf_fillbuf(shf) == -1)
                return (NULL);
            if (shf->rnleft == 0) {
                *buf = '\0';
                return (buf == orig_buf ? NULL : buf);
            }
        }
        end = (unsigned char *)memchr((char *)shf->rp, '\n', shf->rnleft);
        ncopy = end ? end - shf->rp + 1 : shf->rnleft;
        if (ncopy > bsize)
            ncopy = bsize;
        memcpy(buf, shf->rp, ncopy);
        shf->rp += ncopy;
        shf->rnleft -= ncopy;
        buf += ncopy;
        bsize -= ncopy;
    } while (!end && bsize);
    *buf = '\0';
    return (buf);
}

/* Returns the char read. Returns -1 for error and end of file. */
int
shf_getchar(struct shf *shf)
{
    if (!(shf->flags & SHF_RD))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, "shf_getchar", shf->flags);

    if (shf->rnleft == 0 && (shf_fillbuf(shf) == -1 || shf->rnleft == 0))
        return (-1);
    --shf->rnleft;
    return (ord(*shf->rp++));
}

/*
 * Put a character back in the input stream. Returns the character if
 * successful, -1 if there is no room.
 */
int
shf_ungetc(int c, struct shf *shf)
{
    if (!(shf->flags & SHF_RD))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, "shf_ungetc", shf->flags);

    if ((shf->flags & SHF_ERROR) || c == -1 || (shf->rp == shf->buf && shf->rnleft))
        return (-1);

    if ((shf->flags & SHF_WRITING) && shf_emptybuf(shf, EB_READSW) == -1)
        return (-1);

    if (shf->rp == shf->buf)
        shf->rp = shf->buf + shf->rbsize;
    if (shf->flags & SHF_STRING) {
        /*
         * Can unget what was read, but not something different;
         * we don't want to modify a string.
         */
        if ((int)(shf->rp[-1]) != c)
            return (-1);
        shf->flags &= ~SHF_EOF;
        shf->rp--;
        shf->rnleft++;
        return (c);
    }
    shf->flags &= ~SHF_EOF;
    *--(shf->rp) = c;
    shf->rnleft++;
    return (c);
}

/*
 * Write a character. Returns the character if successful, -1 if the
 * char could not be written.
 */
int
shf_putchar(int c, struct shf *shf)
{
    if (!(shf->flags & SHF_WR))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, "shf_putchar", shf->flags);

    if (c == -1)
        return (-1);

    if (shf->flags & SHF_UNBUF) {
        unsigned char cc = (unsigned char)c;
        ssize_t n;

        if (shf->fd < 0)
            kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_TWOMSG | KWF_NOERRNO, "shf_putchar", "no fd");
        if (shf->flags & SHF_ERROR) {
            errno = shf->errnosv;
            return (-1);
        }
        while ((n = write(shf->fd, &cc, 1)) != 1)
            if (n < 0) {
                if (errno == EINTR && !(shf->flags & SHF_INTERRUPT))
                    continue;
                shf->flags |= SHF_ERROR;
                shf->errnosv = errno;
                return (-1);
            }
    } else {
        /* Flush deals with strings and sticky errors */
        if (shf->wnleft == 0 && shf_emptybuf(shf, EB_GROW) == -1)
            return (-1);
        shf->wnleft--;
        *shf->wp++ = c;
    }

    return (c);
}

/*
 * Write a string. Returns the length of the string if successful,
 * less if truncated, and -1 if the string could not be written.
 */
ssize_t
shf_putsv(const char *s, struct shf *shf)
{
    if (!s)
        return (-1);

    return (shf_write(s, strlen(s), shf));
}

/*
 * Write a buffer. Returns nbytes if successful, less if truncated
 * (outputting to string only), and -1 if there is an error.
 */
ssize_t
shf_write(const char *buf, ssize_t nbytes, struct shf *shf)
{
    ssize_t n, ncopy, orig_nbytes = nbytes;

    if (!(shf->flags & SHF_WR))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_flags, Tshf_write, shf->flags);

    if (nbytes < 0)
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_buf, Tshf_write, (size_t)buf,
               nbytes);

    /* don't buffer if buffer is empty and we're writing a large amount */
    if ((ncopy = shf->wnleft) && (shf->wp != shf->buf || nbytes < shf->wnleft)) {
        if (ncopy > nbytes)
            ncopy = nbytes;
        memcpy(shf->wp, buf, ncopy);
        nbytes -= ncopy;
        buf += ncopy;
        shf->wp += ncopy;
        shf->wnleft -= ncopy;
    }
    if (nbytes > 0) {
        if (shf->flags & SHF_STRING) {
            /* resize buffer until there's enough space left */
            while (nbytes > shf->wnleft)
                if (shf_emptybuf(shf, EB_GROW) == -1) {
                    /* truncate if possible */
                    if (shf->wnleft == 0)
                        return (-1);
                    nbytes = shf->wnleft;
                    break;
                }
            /* then write everything into the buffer */
        } else {
            /* flush deals with sticky errors */
            if (shf_emptybuf(shf, EB_GROW) == -1)
                return (-1);
            /* write chunks larger than window size directly */
            if (nbytes > shf->wbsize) {
                ncopy = nbytes;
                if (shf->wbsize)
                    ncopy -= nbytes % shf->wbsize;
                nbytes -= ncopy;
                while (ncopy > 0) {
                    n = write(shf->fd, buf, ncopy);
                    if (n < 0) {
                        if (errno == EINTR && !(shf->flags & SHF_INTERRUPT))
                            continue;
                        shf->flags |= SHF_ERROR;
                        shf->errnosv = errno;
                        shf->wnleft = 0;
                        /*
                         * Note: fwrite(3) returns 0
                         * for errors - this doesn't
                         */
                        return (-1);
                    }
                    buf += n;
                    ncopy -= n;
                }
            }
            /* ... and buffer the rest */
        }
        if (nbytes > 0) {
            /* write remaining bytes to buffer */
            memcpy(shf->wp, buf, nbytes);
            shf->wp += nbytes;
            shf->wnleft -= nbytes;
        }
    }

    return (orig_nbytes);
}

ssize_t
shf_fprintf(struct shf *shf, const char *fmt, ...)
{
    va_list args;
    ssize_t n;

    va_start(args, fmt);
    n = shf_vfprintf(shf, fmt, args);
    va_end(args);

    return (n);
}

ssize_t
shf_snprintf(char *buf, ssize_t bsize, const char *fmt, ...)
{
    struct shf shf;
    va_list args;
    ssize_t n;

    if (!buf || bsize <= 0)
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tbad_buf, "shf_snprintf", (size_t)buf,
               bsize);

    shf_sopen(buf, bsize, SHF_WR, &shf);
    va_start(args, fmt);
    n = shf_vfprintf(&shf, fmt, args);
    va_end(args);
    /* NUL terminates */
    shf_sclose(&shf);
    return (n);
}

char *
shf_smprintf(const char *fmt, ...)
{
    struct shf shf;
    va_list args;

    shf_sopen(NULL, 0, SHF_WR | SHF_DYNAMIC, &shf);
    va_start(args, fmt);
    shf_vfprintf(&shf, fmt, args);
    va_end(args);
    /* NUL terminates */
    return (shf_sclose(&shf));
}

/* pre-initio() */
char *
kslfmt(ksl number, kui flags, char *numbuf)
{
    /* easy for positive number */
    if (number >= 0)
        return (kulfmt((kul)number, flags, numbuf));
    /* negative signed quantity */
    if (!IS(flags, FM_TYPE, FL_SGN)) {
        /* uh-oh, output a signed quantity unsignedly */
        return (kulfmt(qiA_S2U(kul, ksl, number), flags, numbuf));
    }
    return (kulfmt(qiA_S2M(kul, ksl, number), flags | FL_NEG, numbuf));
}

/* pre-initio() */
char *
kulfmt(kul number, kui flags, char *numbuf)
{
    char *cp;

    cp = numbuf + NUMBUFSZ;
    *--cp = '\0';
    switch (flags & FM_TYPE) {
    case FL_OCT:
        do {
            *--cp = digits_lc[number & 07UL];
            number >>= 3;
        } while (number);

        if (HAS(flags, FL_HASH) && ord(*cp) != ORD('0'))
            *--cp = '0';
        break;
    case FL_HEX: {
        const char *digits;

        digits = HAS(flags, FL_UCASE) ? digits_uc : digits_lc;
        do {
            *--cp = digits[number & 0xFUL];
            number >>= 4;
        } while (number);

        if (HAS(flags, FL_HASH)) {
            *--cp = IS(flags, FL_UPPER, FL_UPPER) ? 'X' : 'x';
            *--cp = '0';
        }
        break;
    }
    default:
        do {
            *--cp = digits_lc[number % 10UL];
            number /= 10UL;
        } while (number);

        if (IS(flags, FM_TYPE, FL_SGN)) {
            if (HAS(flags, FL_NEG))
                *--cp = '-';
            else if (HAS(flags, FL_PLUS))
                *--cp = '+';
            else if (HAS(flags, FL_BLANK))
                *--cp = ' ';
        }
        break;
    }

    return (cp);
}

ssize_t
shf_vfprintf(struct shf *shf, const char *fmt, va_list args)
{
    char numbuf[NUMBUFSZ];
    const char *s;
    char c;
    int tmp = 0, flags;
    size_t field, precision, len;
    /* this stuff for dealing with the buffer */
    ssize_t nwritten = 0;
    /* for width determination */
    const char *lp, *np;

#define VA(type) va_arg(args, type)

    if (!fmt)
        return (0);

    while ((c = *fmt++)) {
        if (c != '%') {
            shf_putc(c, shf);
            nwritten++;
            continue;
        }
        /*
         * This will accept flags/fields in any order - not just
         * the order specified in printf(3), but this is the way
         * _doprnt() seems to work (on BSD and SYSV). The only
         * restriction is that the format character must come
         * last :-).
         */
        flags = 0;
        field = precision = 0;
        while ((c = *fmt++)) {
            switch (c) {
            case '#':
                flags |= FL_HASH;
                continue;

            case '+':
                flags |= FL_PLUS;
                continue;

            case '-':
                flags |= FL_RIGHT;
                continue;

            case ' ':
                flags |= FL_BLANK;
                continue;

            case '0':
                if (!(flags & FL_DOT))
                    flags |= FL_ZERO;
                continue;

            case '.':
                flags |= FL_DOT;
                precision = 0;
                continue;

            case '*':
                tmp = VA(int);
                if (tmp < 0) {
                    if (flags & FL_DOT)
                        precision = 0;
                    else {
                        field = (unsigned int)-tmp;
                        flags |= FL_RIGHT;
                    }
                } else if (flags & FL_DOT)
                    precision = (unsigned int)tmp;
                else
                    field = (unsigned int)tmp;
                continue;

            case 'h':
                flags &= ~FM_SIZES;
                flags |= FL_SHORT;
                continue;

            case 'j':
                flags &= ~FM_SIZES;
                flags |= FL_LONG;
                continue;

            case 'l':
                flags &= ~FM_SIZES;
                flags |= FL_LONG;
                continue;

            case 'z':
                flags &= ~FM_SIZES;
                flags |= FL_SIZET;
                continue;
            }
            if (ctype(c, C_DIGIT)) {
                --fmt;
                getpn((const char **)&fmt, &tmp);
                if (flags & FL_DOT)
                    precision = (unsigned int)tmp;
                else
                    field = (unsigned int)tmp;
                continue;
            }
            break;
        }

        if (!c)
            /* nasty format */
            break;

        if (ctype(c, C_UPPER)) {
            flags |= FL_UPPER;
            c = qsh_tolower(c);
        }

        switch (c) {
        case 'd':
        case 'i':
                s = kslfmt(HAS(flags, FL_SIZET)   ? (ksl)VA(ssize_t)
                           : HAS(flags, FL_LONG)  ? (ksl)VA(long)
                           : HAS(flags, FL_SHORT) ? (ksl)(short)VA(int)
                                                  : (ksl)VA(int),
                           flags | FL_SGN, numbuf);
            goto integral;

        case 'o':
            flags |= FL_OCT;
            if (0)
                /* FALLTHROUGH */
            case 'u':
                flags |= FL_DEC;
            if (0)
                /* FALLTHROUGH */
            case 'x':
                flags |= FL_HEX;
                s = kulfmt(HAS(flags, FL_SIZET)   ? (kul)VA(size_t)
                           : HAS(flags, FL_LONG)  ? (kul)VA(unsigned long)
                           : HAS(flags, FL_SHORT) ? (kul)(unsigned short)VA(int)
                                                  : (kul)VA(unsigned int),
                           flags, numbuf);
        integral:
            flags |= FL_NUMBER;
            len = NUMBUFLEN(numbuf, s);
            if (flags & FL_DOT) {
                if (precision > len) {
                    field = precision;
                    flags |= FL_ZERO;
                } else
                    /* no loss */
                    precision = len;
            }
            break;

        case 's':
            if ((s = VA(const char *)) == NULL)
                s = Tnil;
            else if (flags & FL_HASH) {
                print_value_quoted(shf, s);
                continue;
            }
            len = utf_mbswidth(s);
            break;

        case 'c':
            flags &= ~FL_DOT;
            c = (char)(VA(int));
            /* FALLTHROUGH */

        case '%':
        default:
            numbuf[0] = c;
            numbuf[1] = 0;
            s = numbuf;
            len = 1;
            break;
        }

        /*
         * At this point s should point to a string that is to be
         * formatted, and len should be the length of the string.
         */
        if (!(flags & FL_DOT) || len < precision)
            precision = len;
        /* determine whether we can indeed write precision columns */
        len = 0;
        lp = s;
        while (*lp) {
            int w = utf_widthadj(lp, &np);

            if ((len + w) > precision)
                break;
            lp = np;
            len += w;
        }
        /* trailing combining characters */
        if (UTFMODE)
            while (*lp && utf_widthadj(lp, &np) == 0)
                lp = np;
        /* how much we can actually output */
        precision = len;

        /* output leading padding */
        if (field > precision) {
            field -= precision;
            if (!(flags & FL_RIGHT)) {
                /* skip past sign or 0x when padding with 0 */
                if ((flags & (FL_ZERO | FL_NUMBER)) == (FL_ZERO | FL_NUMBER)) {
                    if (ctype(*s, C_SPC | C_PLUS | C_MINUS)) {
                        shf_putc(*s, shf);
                        s++;
                        precision--;
                        nwritten++;
                    } else if (*s == '0') {
                        shf_putc(*s, shf);
                        s++;
                        nwritten++;
                        if (--precision && isCh(*s, 'X', 'x')) {
                            shf_putc(*s, shf);
                            s++;
                            precision--;
                            nwritten++;
                        }
                    }
                    c = '0';
                } else
                    c = flags & FL_ZERO ? '0' : ' ';
                nwritten += field;
                while (field--)
                    shf_putc(c, shf);
                field = 0;
            } else
                c = ' ';
        } else
            field = 0;

        /* output string */
        nwritten += (lp - s);
        shf_write(s, lp - s, shf);

        /* output trailing padding */
        nwritten += field;
        while (field--)
            shf_putc(c, shf);
    }

    return (shf_error(shf) ? -1 : nwritten);
}

/* fast character classes */
const kui tpl_ctypes[128] = {
    /* 0x00 */
    CiNUL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiTAB, CiNL,
    CiSPX, CiSPX, CiCR, CiCNTRL, CiCNTRL,
    /* 0x10 */
    CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL,
    CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL, CiCNTRL,
    /* 0x20 */
    CiSP, CiALIAS | CiVAR1, CiQC, CiHASH, CiSS, CiPERCT, CiQCL, CiQC, CiQCL, CiQCL, CiQCX | CiVAR1,
    CiPLUS, CiALIAS, CiMINUS, CiALIAS, CiQCM,
    /* 0x30 */
    CiOCTAL, CiOCTAL, CiOCTAL, CiOCTAL, CiOCTAL, CiOCTAL, CiOCTAL, CiOCTAL, CiDIGIT, CiDIGIT,
    CiCOLON, CiQCL, CiANGLE, CiEQUAL, CiANGLE, CiQUEST,
    /* 0x40 */
    CiALIAS | CiVAR1, CiUPPER | CiHEXLT, CiUPPER | CiHEXLT, CiUPPER | CiHEXLT, CiUPPER | CiHEXLT,
    CiUPPER | CiHEXLT, CiUPPER | CiHEXLT, CiUPPER, CiUPPER, CiUPPER, CiUPPER, CiUPPER, CiUPPER,
    CiUPPER, CiUPPER, CiUPPER,
    /* 0x50 */
    CiUPPER, CiUPPER, CiUPPER, CiUPPER, CiUPPER, CiUPPER, CiUPPER, CiUPPER, CiUPPER, CiUPPER,
    CiUPPER, CiQCX | CiBRACK, CiQCX, CiBRACK, CiQCM, CiUNDER,
    /* 0x60 */
    CiGRAVE, CiLOWER | CiHEXLT, CiLOWER | CiHEXLT, CiLOWER | CiHEXLT, CiLOWER | CiHEXLT,
    CiLOWER | CiHEXLT, CiLOWER | CiHEXLT, CiLOWER, CiLOWER, CiLOWER, CiLOWER, CiLOWER, CiLOWER,
    CiLOWER, CiLOWER, CiLOWER,
    /* 0x70 */
    CiLOWER, CiLOWER, CiLOWER, CiLOWER, CiLOWER, CiLOWER, CiLOWER, CiLOWER, CiLOWER, CiLOWER,
    CiLOWER, CiCURLY, CiQCL, CiCURLY, CiQCX, CiCNTRL};

/* pre-initio() */
static void
set_ccls(void)
{
    memcpy(qsh_ctypes, tpl_ctypes, sizeof(tpl_ctypes));
    memset((char *)qsh_ctypes + sizeof(tpl_ctypes), '\0', sizeof(qsh_ctypes) - sizeof(tpl_ctypes));
}

/* pre-initio() */
void
set_ifs(const char *s)
{
    set_ccls();
    ifs0 = *s;
    while (*s)
        qsh_ctypes[ord(*s++)] |= CiIFS;
}


