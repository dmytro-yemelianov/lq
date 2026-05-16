/*
 * /etc/shadow parser.
 *
 * Implements:
 *   setspent, getspent, endspent, getspnam
 *
 * Format (one record per line):
 *   name:passwd:lstchg:min:max:warn:inact:expire:flag
 *
 * Empty numeric fields are returned as -1 (the canonical "unset"
 * sentinel for the long fields in struct spwd).  Any field may be
 * empty in real /etc/shadow files.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 */

#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SHADOW_PATH    "/etc/shadow"
#define SHADOW_LINEMAX 512

static FILE        *sp_fp;
static struct spwd  sp_entry;
static char         sp_line[SHADOW_LINEMAX];

static char *
next_field(char **sp)
{
    char *start = *sp;
    char *p     = start;

    while (*p && *p != ':')
        p++;
    if (*p == ':')
        *p++ = '\0';
    *sp = p;
    return start;
}

/* Convert a numeric shadow field to long; an empty string means -1. */
static long
shadow_long(const char *s)
{
    if (!s || !*s)
        return -1L;
    return strtol(s, NULL, 10);
}

void
setspent(void)
{
    if (sp_fp) {
        fseek(sp_fp, 0L, SEEK_SET);
        clearerr(sp_fp);
        return;
    }
    sp_fp = fopen(SHADOW_PATH, "r");
}

void
endspent(void)
{
    if (sp_fp) {
        fclose(sp_fp);
        sp_fp = NULL;
    }
}

struct spwd *
getspent(void)
{
    char *line, *eol;

    if (!sp_fp) {
        setspent();
        if (!sp_fp)
            return NULL;
    }

    for (;;) {
        if (!fgets(sp_line, sizeof(sp_line), sp_fp))
            return NULL;

        eol = sp_line + strlen(sp_line);
        while (eol > sp_line && (eol[-1] == '\n' || eol[-1] == '\r'))
            *--eol = '\0';

        if (sp_line[0] == '\0' || sp_line[0] == '#')
            continue;

        if (eol == sp_line + sizeof(sp_line) - 1) {
            errno = ERANGE;
            return NULL;
        }

        line = sp_line;
        sp_entry.sp_namp = next_field(&line);
        sp_entry.sp_pwdp = next_field(&line);
        sp_entry.sp_lstchg = shadow_long(next_field(&line));
        sp_entry.sp_min    = shadow_long(next_field(&line));
        sp_entry.sp_max    = shadow_long(next_field(&line));
        sp_entry.sp_warn   = shadow_long(next_field(&line));
        sp_entry.sp_inact  = shadow_long(next_field(&line));
        sp_entry.sp_expire = shadow_long(next_field(&line));
        sp_entry.sp_flag   = shadow_long(next_field(&line));

        if (sp_entry.sp_namp[0] == '\0')
            continue;
        return &sp_entry;
    }
}

struct spwd *
getspnam(const char *name)
{
    struct spwd *sp;

    if (!name)
        return NULL;
    setspent();
    while ((sp = getspent()) != NULL) {
        if (strcmp(sp->sp_namp, name) == 0) {
            endspent();
            return sp;
        }
    }
    endspent();
    return NULL;
}
