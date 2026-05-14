/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"
#include "qsh_hash.h"

/* Persistent-history file machinery — split out of history.c.
 * In-memory history lives there; on-disk format lives here. */

/* Cross-file state (definitions live in history.c). */
extern bool hstarted;
extern Source *hist_source;
extern char **current;

/* histsave() svmode marker shared with history.c.  Defined in both
 * files; if either definition changes, change both. */
#define HIST_DISCARD 5

/* OE-portability shims used by the mmap-based loader. */
#define caddr_cast(x) ((caddr_t)(x))
#ifndef MAP_FAILED
#define MAP_FAILED caddr_cast(-1)
#endif
#ifndef MAP_FILE
#define MAP_FILE 0
#endif

/* Current persistent-history file: name, fd, size.  Used here and
 * referenced via extern from history.c when it needs to know whether
 * persistent history is active. */
char *hname;
int histfd = -1;
off_t histfsize;

/*-
 * Open a history file
 * Format is:
 * Bytes 1, 2:
 *  HMAGIC - just to check that we are dealing with the correct object
 * Then follows a number of stored commands
 * Each command is
 *  <command byte><command number(4 octets, big endian)><bytes><NUL>
 */
#define HMAGIC1 0xAB
#define HMAGIC2 0xCD
#define COMMAND 0xFF

void
hist_init(Source *s)
{
    histsave(NULL, NULL, HIST_DISCARD, true);

    if (Flag(FTALKING) == 0)
        return;

    hstarted = true;
    hist_source = s;

}

