/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Temporary-file allocation (here-docs, $(...), etc.).
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

static const char temp_tpl[] = "/shXXXXXX.tmp";

struct temp *
maketemp(Area *ap, Temp_type type, struct temp **tlist)
{
    char *cp;
    size_t len;
    int i, j;
    struct temp *tp;
    const char *dir;
    struct stat sb;

    dir = tmpdir ? tmpdir : QSH_DEFAULT_TMPDIR;
    /* add "/shXXXXXX.tmp" plus NUL */
    len = strlen(dir);
    cp = alloc1(qccFAMSZ(struct temp, tffn, sizeof(temp_tpl)), len, ap);

    tp = (void *)cp;
    tp->shf = NULL;
    tp->pid = procpid;
    tp->type = type;

    cp += offsetof(struct temp, tffn);
    memcpy(cp, dir, len);
    cp += len;
    memstr(cp, temp_tpl);

    if (stat(dir, &sb) || !S_ISDIR(sb.st_mode))
        goto maketemp_out;

    /* point to the first of six Xes */
    cp += 3;

    /* cyclically attempt to open a temporary file */
    do {
        /* generate random part of filename */
        len = 0;
        do {
            cp[len++] = digits_lc[rndget() % 36];
        } while (len < 6);

        /* check if this one works */
        if ((i = binopen3(tp->tffn, O_CREAT | O_EXCL | O_RDWR, 0600)) < 0 && errno != EEXIST)
            goto maketemp_out;
    } while (i < 0);

    if (type == TT_FUNSUB) {
        /* map us high and mark as close-on-exec */
        if ((j = savefd(i)) != i) {
            close(i);
            i = j;
        }

        /* operation mode for the shf */
        j = SHF_RD;
    } else
        j = SHF_WR;

    /* shf_fdopen cannot fail, so no fd leak */
    tp->shf = shf_fdopen(i, j, NULL);

maketemp_out:
    tp->next = *tlist;
    *tlist = tp;
    return (tp);
}
