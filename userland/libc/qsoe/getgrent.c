/*
 * /etc/group parser.
 *
 * Implements:
 *   setgrent, getgrent, endgrent, getgrnam, getgrgid
 *
 * Format (POSIX, one record per line):
 *   name:passwd:gid:user1,user2,...
 *
 * The members list is split in place; gr_mem points into a static
 * pointer array that gets rebuilt on every call.  Same trade-offs
 * as getpwent: not thread-safe; reentrant variants TBD.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 */

#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define GROUP_PATH        "/etc/group"
#define GROUP_LINEMAX     512
#define GROUP_MAXMEMBERS  64        /* members per group */

static FILE         *gr_fp;
static struct group  gr_entry;
static char          gr_line[GROUP_LINEMAX];
static char         *gr_members[GROUP_MAXMEMBERS + 1];   /* +1 for NULL terminator */

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

void
setgrent(void)
{
    if (gr_fp) {
        fseek(gr_fp, 0L, SEEK_SET);
        clearerr(gr_fp);
        return;
    }
    gr_fp = fopen(GROUP_PATH, "r");
}

void
endgrent(void)
{
    if (gr_fp) {
        fclose(gr_fp);
        gr_fp = NULL;
    }
}

struct group *
getgrent(void)
{
    char *p, *line, *eol, *members;
    size_t nmem;

    if (!gr_fp) {
        setgrent();
        if (!gr_fp)
            return NULL;
    }

    for (;;) {
        if (!fgets(gr_line, sizeof(gr_line), gr_fp))
            return NULL;

        eol = gr_line + strlen(gr_line);
        while (eol > gr_line && (eol[-1] == '\n' || eol[-1] == '\r'))
            *--eol = '\0';

        if (gr_line[0] == '\0' || gr_line[0] == '#')
            continue;

        if (eol == gr_line + sizeof(gr_line) - 1) {
            errno = ERANGE;
            return NULL;
        }

        line = gr_line;
        gr_entry.gr_name   = next_field(&line);
        gr_entry.gr_passwd = next_field(&line);

        p = next_field(&line);
        gr_entry.gr_gid = (gid_t)strtoul(p, NULL, 10);

        /* Members: comma-separated.  Split in place. */
        members = next_field(&line);
        nmem = 0;
        if (*members) {
            char *m = members;
            char *q;

            gr_members[nmem++] = m;
            for (q = m; *q; q++) {
                if (*q == ',') {
                    *q = '\0';
                    if (nmem >= GROUP_MAXMEMBERS) {
                        errno = ERANGE;
                        return NULL;
                    }
                    gr_members[nmem++] = q + 1;
                }
            }
        }
        gr_members[nmem] = NULL;
        gr_entry.gr_mem  = gr_members;

        if (gr_entry.gr_name[0] == '\0')
            continue;

        return &gr_entry;
    }
}

struct group *
getgrnam(const char *name)
{
    struct group *gr;

    if (!name)
        return NULL;
    setgrent();
    while ((gr = getgrent()) != NULL) {
        if (strcmp(gr->gr_name, name) == 0) {
            endgrent();
            return gr;
        }
    }
    endgrent();
    return NULL;
}

struct group *
getgrgid(gid_t gid)
{
    struct group *gr;

    setgrent();
    while ((gr = getgrent()) != NULL) {
        if (gr->gr_gid == gid) {
            endgrent();
            return gr;
        }
    }
    endgrent();
    return NULL;
}
