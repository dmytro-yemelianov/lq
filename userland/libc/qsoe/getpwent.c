/*
 * /etc/passwd parser.
 *
 * Implements:
 *   setpwent, getpwent, endpwent, getpwnam, getpwuid
 *
 * Format (POSIX, one record per line):
 *   name:passwd:uid:gid:gecos:home:shell
 *
 * The returned struct passwd's char* fields point into a static
 * line buffer; subsequent calls overwrite it.  Not thread-safe
 * (matches POSIX getpwent).  The reentrant getpwnam_r/getpwuid_r
 * variants are not yet implemented — easy follow-up if needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 */

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define PASSWD_PATH    "/etc/passwd"
#define PASSWD_LINEMAX 512

static FILE          *pw_fp;
static struct passwd  pw_entry;
static char           pw_line[PASSWD_LINEMAX];

/*
 * Split `s` at the next ':' (or NUL).  Returns the start of the
 * field; sets *s to point past the separator (or to the trailing
 * NUL).  Replaces ':' with '\0' in place.
 */
static char *
next_field(char **sp)
{
    char *start = *sp;
    char *p     = start;

    while (*p && *p != ':')
        p++;
    if (*p == ':') {
        *p++ = '\0';
    }
    *sp = p;
    return start;
}

void
setpwent(void)
{
    if (pw_fp) {
        fseek(pw_fp, 0L, SEEK_SET);
        clearerr(pw_fp);
        return;
    }
    pw_fp = fopen(PASSWD_PATH, "r");
}

void
endpwent(void)
{
    if (pw_fp) {
        fclose(pw_fp);
        pw_fp = NULL;
    }
}

struct passwd *
getpwent(void)
{
    char *p, *line, *eol;

    if (!pw_fp) {
        setpwent();
        if (!pw_fp)
            return NULL;
    }

    for (;;) {
        if (!fgets(pw_line, sizeof(pw_line), pw_fp))
            return NULL;

        /* Strip trailing newline (and any \r before it) */
        eol = pw_line + strlen(pw_line);
        while (eol > pw_line && (eol[-1] == '\n' || eol[-1] == '\r'))
            *--eol = '\0';

        /* Skip blank lines and comments */
        if (pw_line[0] == '\0' || pw_line[0] == '#')
            continue;

        /* If the line didn't fit in our buffer (no terminator before
         * EOF and no \n stripped), refuse — partial fields would be
         * worse than a missing entry. */
        if (eol == pw_line + sizeof(pw_line) - 1) {
            errno = ERANGE;
            return NULL;
        }

        line = pw_line;
        pw_entry.pw_name   = next_field(&line);
        pw_entry.pw_passwd = next_field(&line);

        p = next_field(&line);
        pw_entry.pw_uid = (uid_t)strtoul(p, NULL, 10);

        p = next_field(&line);
        pw_entry.pw_gid = (gid_t)strtoul(p, NULL, 10);

        pw_entry.pw_gecos = next_field(&line);
        pw_entry.pw_dir   = next_field(&line);
        pw_entry.pw_shell = next_field(&line);

        /* No pw_age / pw_comment here — musl's struct passwd, the
         * v0.8 surface, omits them.  Those SVR4/QNX legacy fields
         * may come back when QSOE's own libc replaces musl
         * (~v0.10-v0.15). */

        /* A name is the only required field — empty -> skip */
        if (pw_entry.pw_name[0] == '\0')
            continue;

        return &pw_entry;
    }
}

struct passwd *
getpwnam(const char *name)
{
    struct passwd *pw;

    if (!name)
        return NULL;
    setpwent();
    while ((pw = getpwent()) != NULL) {
        if (strcmp(pw->pw_name, name) == 0) {
            endpwent();
            return pw;
        }
    }
    endpwent();
    return NULL;
}

struct passwd *
getpwuid(uid_t uid)
{
    struct passwd *pw;

    setpwent();
    while ((pw = getpwent()) != NULL) {
        if (pw->pw_uid == uid) {
            endpwent();
            return pw;
        }
    }
    endpwent();
    return NULL;
}
