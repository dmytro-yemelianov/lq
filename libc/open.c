/*
 * open.c — POSIX open() for QSOE.
 *
 * Pre-processing before TM_REQ_OPEN goes to taskman:
 *
 *   1. If the path is relative, prepend the calling process's cwd
 *      (TM_REQ_GETCWD) so taskman's pathmgr always sees an absolute
 *      path.  Pathmgr is keyed on registered prefixes ("/", "/bin",
 *      "/dev/null", ...) and has no concept of "." / cwd itself.
 *
 *   2. Canonicalise: collapse "." and ".." components, fold "//" to
 *      "/".  Without this, "ls" from cwd="/" builds "/." which is
 *      unregistered → ENOENT; "cd dev && ls" builds "/dev/." → same
 *      failure.  Canonicalisation runs in a fixed-size buffer;
 *      longer paths return ENAMETOOLONG.
 *
 * After both passes, the absolute, canonical path goes into
 * msg[4..] and TM_REQ_OPEN runs as before.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/qsoe.h>
#include <qsoe/slots.h>
#include <qsoe/tm_msgs.h>
#include "state.h"
#include <sel4_types.h>
#include <qsoe_invoke.h>

extern char *getcwd(char *buf, unsigned long size);
extern int close(int fd);

#define PATH_BUF_BYTES 256

static unsigned path_strlen(const char *s)
{
    unsigned n = 0;
    while (s[n]) ++n;
    return n;
}

/* Canonicalise an absolute path in place.  Splits on '/', walks
 * components, drops "" (consecutive slashes) and ".", pops the
 * previous component on "..".  Uses an explicit input length so the
 * output cursor's writes (which include a trailing '/' after each
 * real component) can safely clobber what used to be the input's
 * NUL terminator without confusing the input-read loop.
 *
 * Returns 0 on success, -1 on a non-absolute input.  *out_len gets
 * the new length on success; buf is NUL-terminated. */
static int canon_inplace(char *buf, unsigned in_len, unsigned *out_len)
{
    if (in_len == 0 || buf[0] != '/') return -1;

    /* Two cursors, both indices into buf.  `o` only writes; `i` only
     * reads from the original input region [0, in_len).  Writes
     * cannot extend past `o`, and `o` never overtakes `i` (each
     * iteration writes ≤ (i - o) + len + 1 bytes ≤ component bytes
     * already read).  Past the explicit end-of-input, *i would be
     * undefined — the `i < in_len` guards prevent that. */
    unsigned o = 0;
    unsigned i = 0;

    buf[o++] = '/';
    ++i;                              /* skip the leading '/' */

    while (i < in_len) {
        unsigned comp = i;
        while (i < in_len && buf[i] != '/') ++i;
        unsigned len = i - comp;

        if (len == 0) {
            /* "//" — empty component, drop. */
        } else if (len == 1 && buf[comp] == '.') {
            /* ".": drop. */
        } else if (len == 2 && buf[comp] == '.' && buf[comp + 1] == '.') {
            /* "..": pop the previous component (or stay at root). */
            if (o > 1) {
                --o;                 /* drop trailing '/' */
                while (o > 1 && buf[o - 1] != '/') --o;
            }
        } else {
            /* Real component — append, then a trailing '/'.  Read
             * each byte BEFORE we write at `o`, since o ≤ i and a
             * write at o could be at the comp source byte. */
            for (unsigned k = 0; k < len; ++k) {
                char c = buf[comp + k];   /* read first */
                buf[o++] = c;             /* then write */
            }
            buf[o++] = '/';
        }

        if (i < in_len && buf[i] == '/') ++i;
    }

    /* Strip trailing '/' unless we're at the bare root. */
    if (o > 1 && buf[o - 1] == '/') --o;
    buf[o]   = 0;
    *out_len = o;
    return 0;
}

int open(const char *path, int flags, ...);
int open(const char *path, int flags, ...)
{
    if (!path) { qsoe_errno = EINVAL; return -1; }

    /* Stage 1 — assemble an absolute path. */
    char abs[PATH_BUF_BYTES];
    unsigned alen = 0;

    if (path[0] == '/') {
        unsigned plen = path_strlen(path);
        if (plen >= sizeof abs) { qsoe_errno = ENAMETOOLONG; return -1; }
        for (unsigned i = 0; i < plen; ++i) abs[i] = path[i];
        abs[plen] = 0;
        alen = plen;
    } else {
        if (!getcwd(abs, sizeof abs)) return -1;
        unsigned cwdlen = 0;
        while (abs[cwdlen] && cwdlen < sizeof abs) ++cwdlen;
        /* Add separator unless cwd is the bare "/". */
        if (!(cwdlen == 1 && abs[0] == '/')) {
            if (cwdlen + 1 >= sizeof abs) { qsoe_errno = ENAMETOOLONG; return -1; }
            abs[cwdlen++] = '/';
        }
        unsigned i = 0;
        while (path[i] && cwdlen + i < sizeof abs - 1) {
            abs[cwdlen + i] = path[i];
            ++i;
        }
        if (path[i] != 0) { qsoe_errno = ENAMETOOLONG; return -1; }
        abs[cwdlen + i] = 0;
        alen = cwdlen + i;
    }

    /* Stage 2 — canonicalise. */
    if (canon_inplace(abs, alen, &alen) != 0) {
        qsoe_errno = EINVAL;
        return -1;
    }
    if (alen == 0 || alen >= 128) { qsoe_errno = EINVAL; return -1; }

    /* Stage 3 — TM_REQ_OPEN. */
    unsigned char *dst = (unsigned char *)&qsoe_ipcbuf->msg[4];
    for (unsigned i = 0; i < alen; ++i) dst[i] = (unsigned char)abs[i];

    seL4_Word mr0 = alen, mr1 = 0, mr2 = 0, mr3 = 0;
    unsigned nwords = 4 + (alen + 7) / 8;
    seL4_MessageInfo_t tag = seL4_MessageInfo_new(TM_REQ_OPEN, 0, 0, nwords);
    seL4_MessageInfo_t reply = qsoe_sys_call(QSOE_CAP_TASKMAN_EP, tag,
                                              &mr0, &mr1, &mr2, &mr3);
    seL4_Word err = seL4_MessageInfo_get_label(reply);
    if (err != 0) { qsoe_errno = (int)err; return -1; }

    /* If taskman followed a cross-fs symlink (/etc -> /usr/conf etc.) it
     * returned the rewritten path in msg[4..] with mr2 = its length.
     * Capture it NOW, before any further IPC reuses the buffer; this is the
     * path the external resmgr must see on _IO_CONNECT (it keys on the path
     * string).  No rewrite (mr2 == 0) -> use the canonical path we sent. */
    char     conn_path[128];
    unsigned conn_len;
    if (mr2 > 0 && (unsigned)mr2 < sizeof conn_path) {
        const unsigned char *rs = (const unsigned char *)&qsoe_ipcbuf->msg[4];
        conn_len = (unsigned)mr2;
        for (unsigned i = 0; i < conn_len; ++i) conn_path[i] = (char)rs[i];
    } else {
        conn_len = alen;
        for (unsigned i = 0; i < alen; ++i) conn_path[i] = abs[i];
    }

    seL4_CPtr cap_slot = (seL4_CPtr)mr0;
    int fd = qsoe_state_alloc_coid(0);
    if (fd < 0) { qsoe_errno = ENOMEM; return -1; }
    qsoe_state_bind_coid(fd, cap_slot);

    /* mr1 != 0: the path resolved to an EXTERNAL resmgr (a libressrv
     * server, not one of taskman's synthetic handlers).  taskman minted
     * the connection cap but cannot run the server's acquire(); send
     * _IO_CONNECT on the fd ourselves so the per-open handle exists before
     * the first read/lseek.  taskman-internal handlers already staged their
     * per-fd state during TM_REQ_OPEN and need no _IO_CONNECT. */
    if (mr1) {
        unsigned char cbuf[sizeof(tm_req_io_connect_t) + 128];
        tm_req_io_connect_t *cc = (tm_req_io_connect_t *)cbuf;
        cc->type        = _IO_CONNECT;
        cc->plen        = conn_len;
        cc->flags       = (unsigned long)flags;
        cc->mode        = 0;
        cc->_reserved[0] = 0;
        for (unsigned i = 0; i < conn_len; ++i)
            cbuf[sizeof(tm_req_io_connect_t) + i] = (unsigned char)conn_path[i];
        int st = MsgSend(fd, cbuf,
                         (int)(sizeof(tm_req_io_connect_t) + conn_len), 0, 0);
        if (st != 0) {              /* acquire() rejected the open */
            close(fd);
            qsoe_errno = st;
            return -1;
        }
    }
    return fd;
}
