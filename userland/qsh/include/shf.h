/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * shf — buffered I/O on top of a single fd or a pre-allocated
 * memory string.  mksh uses this in preference to <stdio.h> for
 * everything except the very early-startup messages, because shf
 * is reentrant (allocates from a caller-supplied Area), keeps
 * the read and write pointers as separate fields (so a duplex
 * stream is one struct), and offers shf_smprintf() / shf_snprintf()
 * with mksh's own %S, %T, %R format specifiers.
 *
 * Implementation lives in shf.c.
 */

#ifndef _QRV_SH_SHF_H
#define _QRV_SH_SHF_H

#include "sh.h"     /* Area, QSH_A_FORMAT/A_BOUNDED, kby, ssize_t */

/* -----------------------------------------------------------------
 * shf — the stream object.
 * ----------------------------------------------------------------- */

#define SHF_BSIZE       512     /* default buffer size */

struct shf {
    Area           *areap;      /* area shf/buf were allocated in */
    unsigned char  *rp;         /* read: current position in buffer */
    unsigned char  *wp;         /* write: current position in buffer */
    unsigned char  *buf;        /* buffer */
    ssize_t         bsize;      /* actual size of buf */
    ssize_t         rbsize;     /* size of read buffer (1 if SHF_UNBUF) */
    ssize_t         rnleft;     /* read: bytes left in buffer */
    ssize_t         wbsize;     /* size of write buffer (0 if SHF_UNBUF) */
    ssize_t         wnleft;     /* write: space left in buffer */
    int             flags;      /* see SHF_* below */
    int             fd;         /* underlying file descriptor */
    int             errnosv;    /* saved errno after error */
};

/* -----------------------------------------------------------------
 * SHF_* flags.  The first set is passed to shf_*open(); the second
 * is internal bookkeeping.
 * ----------------------------------------------------------------- */

/* mode flags (open) */
#define SHF_RD          0x0001
#define SHF_WR          0x0002
#define SHF_RDWR        (SHF_RD | SHF_WR)
#define SHF_ACCMODE     0x0003  /* mask for the three above */
#define SHF_GETFL       0x0004  /* fcntl() to discover RD/WR */
#define SHF_UNBUF       0x0008  /* unbuffered I/O */
#define SHF_CLEXEC      0x0010  /* set close-on-exec */
#define SHF_MAPHI       0x0020  /* shf_open: dup fd above FDBASE, close orig */
#define SHF_DYNAMIC     0x0040  /* string: grow buffer as needed */
#define SHF_INTERRUPT   0x0080  /* EINTR in read/write -> error */

/* internal-state flags */
#define SHF_STRING      0x0100  /* string-backed, not fd-backed */
#define SHF_ALLOCS      0x0200  /* shf itself was alloc()'d */
#define SHF_ALLOCB      0x0400  /* shf->buf was alloc()'d */
#define SHF_ERROR       0x0800  /* read()/write() saw an error */
#define SHF_EOF         0x1000  /* read EOF (sticky) */
#define SHF_READING     0x2000  /* in read state: rp/rnleft valid */
#define SHF_WRITING     0x4000  /* in write state: wp/wnleft valid */

/* -----------------------------------------------------------------
 * Pre-allocated streams (defined in shf.c).
 *
 * shl_stdout / shl_out / shl_dbg are per-process; shl_xtrace gets
 * the set -x output.  initio_done flips to 1 once they've all been
 * wired up — early-startup error paths must therefore go through
 * write(2,…) directly, not through these.
 * ----------------------------------------------------------------- */

extern struct shf shf_iob[];

#define shl_xtrace      (&shf_iob[0])   /* set -x output */
#define shl_stdout      (&shf_iob[1])
#define shl_out         (&shf_iob[2])
#define shl_dbg         (&shf_iob[3])   /* DF() / dbgprintf() */

/* shl_stdout_ok lives in main.c; declared in sh.h. */

/* -----------------------------------------------------------------
 * Inline accessors.
 * ----------------------------------------------------------------- */

#define shf_fileno(shf)             ((shf)->fd)
#define shf_setfileno(shf, nfd)     ((shf)->fd = (nfd))
#define shf_eof(shf)                ((shf)->flags & SHF_EOF)
#define shf_error(shf)              ((shf)->flags & SHF_ERROR)
#define shf_errno(shf)              ((shf)->errnosv)
#define shf_clearerr(shf)           ((shf)->flags &= ~(SHF_EOF | SHF_ERROR))

/* fast paths — fall through to the slow function when buffer is empty/full */
#define shf_getc_i(shf)                                                                    \
    ((shf)->rnleft > 0                                                                     \
     ? ((shf)->rnleft--, (int)ord(*(shf)->rp++))                                           \
     : shf_getchar(shf))
#define shf_putc_i(c, shf)                                                                 \
    ((shf)->wnleft == 0                                                                    \
     ? shf_putchar((kby)(c), (shf))                                                        \
     : ((shf)->wnleft--, *(shf)->wp++ = (c)))
#define shf_getc                shf_getc_i
#define shf_putc                shf_putc_i

#define shf_puts(s, shf)        ((s) ? shf_write((s), strlen(s), (shf)) : (ssize_t)-1)
#define shf_scheck(n, shf)                                                                 \
    (((shf)->wnleft < (ssize_t)(n)) ? shf_scheck_grow((n), (shf)) : 0)

/*
 * Atomic short write: scheck-grow if needed, then memcpy-or-loop.
 * The arguments may be evaluated multiple times in the loop branch;
 * callers tolerate that because n is a one-or-two-byte multibyte
 * char in practice.  Sets s += n; may set n = 0.
 */
#define shf_wr_sm(s, n, shf)                                                               \
    do {                                                                                   \
        if ((shf)->wnleft < (ssize_t)(n)) {                                                \
            shf_scheck_grow((n), (shf));                                                   \
            shf_write((const void *)(s), (n), (shf));                                      \
            (s) += (n);                                                                    \
        } else {                                                                           \
            (shf)->wnleft -= n;                                                            \
            while ((n)--)                                                                  \
                *(shf)->wp++ = *(s)++;                                                     \
        }                                                                                  \
    } while (/* CONSTCOND */ 0)

/* -----------------------------------------------------------------
 * Open / close.
 * ----------------------------------------------------------------- */

struct shf *shf_open(const char *path, int oflags, int mode, int sflags);
struct shf *shf_fdopen(int fd, int sflags, struct shf *);
struct shf *shf_reopen(int fd, int sflags, struct shf *);
struct shf *shf_sopen(char *buf, ssize_t bsize, int sflags, struct shf *);
struct shf *shf_sreopen(char *buf, ssize_t bsize, Area *, struct shf *);

int   shf_close(struct shf *);
int   shf_fdclose(struct shf *);
char *shf_sclose(struct shf *);
int   shf_flush(struct shf *);

/* -----------------------------------------------------------------
 * Read / write.
 * ----------------------------------------------------------------- */

ssize_t shf_read(char *, ssize_t, struct shf *);
char   *shf_getse(char *, ssize_t, struct shf *);
int     shf_getchar(struct shf *);
int     shf_ungetc(int, struct shf *);
int     shf_putchar(int, struct shf *);
ssize_t shf_putsv(const char *, struct shf *);
ssize_t shf_write(const char *, ssize_t, struct shf *);
int     shf_scheck_grow(ssize_t, struct shf *);

/* -----------------------------------------------------------------
 * Formatted output — including mksh's own %S/%T/%R extensions.
 * ----------------------------------------------------------------- */

ssize_t shf_fprintf(struct shf *, const char *, ...)
        QSH_A_FORMAT(__printf__, 2, 3);
ssize_t shf_snprintf(char *, ssize_t, const char *, ...)
        QSH_A_FORMAT(__printf__, 3, 4)
        QSH_A_BOUNDED(__string__, 1, 2);
char   *shf_smprintf(const char *, ...)
        QSH_A_FORMAT(__printf__, 1, 2);
ssize_t shf_vfprintf(struct shf *, const char *, va_list)
        QSH_A_FORMAT(__printf__, 2, 0);

/* -----------------------------------------------------------------
 * Number-format flags (FL_* / FM_*) and the kulfmt / kslfmt helpers
 * that shf_vfprintf uses internally.  Public because mksh callers
 * (e.g. arithmetic builtins) format numbers directly without going
 * through shf.
 * ----------------------------------------------------------------- */

/* request flags */
#define FL_SGN          0x0000  /* signed decimal */
#define FL_DEC          0x0001  /* unsigned decimal */
#define FL_OCT          0x0002  /* unsigned octal */
#define FL_HEX          0x0003  /* unsigned hex */
#define FM_TYPE         0x0003  /* mask: dec/oct/hex */
#define FL_UCASE        0x0004  /* upper-case digits beyond 9 */
#define FL_UPPER        0x000C  /* upper-case digits + 'X' */
#define FL_PLUS         0x0010  /* FL_SGN: force sign output */
#define FL_BLANK        0x0020  /* FL_SGN: leading space if no sign */
#define FL_HASH         0x0040  /* FL_OCT: force 0; FL_HEX: 0x/0X prefix */
/* internal flags, also documented for completeness */
#define FL_NEG          0x0080  /* negative number (negated) */
#define FL_SHORT        0x0100  /* 'h' length modifier */
#define FL_LONG         0x0200  /* 'l' length modifier */
#define FL_SIZET        0x0400  /* 'z' length modifier */
#define FM_SIZES        0x0F00  /* mask: short/long/sizet/huge */
#define FL_RIGHT        0x1000  /* '-' seen: right-pad */
#define FL_ZERO         0x2000  /* '0' seen: zero-pad */
#define FL_DOT          0x4000  /* '.' seen: precision specified */
#define FL_NUMBER       0x8000  /* %[douxefg] formatted */

/*
 * Buffer big enough for the longest possible numeric format.
 * %#o produces '0' + ceil(bits/3) digits + NUL.
 */
#define NUMBUFSZ        (1U + (qiTYPE_UBITS(kuH) + 2U) / 3U + 1U)
#define NUMBUFLEN(base, result)  ((base) + NUMBUFSZ - (result) - 1U)

char *kulfmt(kul number, kui flags, char *numbuf)
        QSH_A_BOUNDED(__minbytes__, 3, NUMBUFSZ);
char *kslfmt(ksl number, kui flags, char *numbuf)
        QSH_A_BOUNDED(__minbytes__, 3, NUMBUFSZ);
#define kuHfmt          kulfmt
#define ksHfmt          kslfmt

#endif /* _QRV_SH_SHF_H */
