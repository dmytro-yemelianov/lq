/*
 * getopt.c — POSIX getopt(3), byte-oriented.
 *
 * Replaces upstream musl's locale-aware getopt (excluded via
 * placement.txt because it transitively pulls in
 * locale_impl.h → multibyte machinery we don't want).  This version
 * is the same algorithm without the mbtowc step: arguments are
 * treated as ASCII / single-byte characters.  Adequate for utils
 * that parse single-letter flags (ls -la, cat -n, etc.); upgrade if
 * a tool ever needs UTF-8 option letters.
 *
 * Behaviour matches POSIX:
 *   - Returns the next option character, or -1 at end of options.
 *   - "-" by itself is treated as a non-option argument and stops
 *     option parsing.
 *   - "--" stops option parsing.
 *   - Leading "-" in optstring enables permuting / argv[i] returns;
 *     leading ":" suppresses error messages.
 *
 * Externals (`optarg`, `optind`, `opterr`, `optopt`) live here too
 * so a single object file resolves the whole API.
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <string.h>

char *optarg;
int   optind = 1;
int   opterr = 1;
int   optopt;

/* Position within argv[optind] for grouped flags ("-la").  Hidden
 * from callers; reset to 0 between arguments. */
static int optpos;

static void emit_unknown(const char *prog, const char *msg, int c)
{
    /* Best-effort error report on stderr.  No FILE locking — tools
     * that need atomic concurrent stderr writes can roll their own. */
    if (prog) {
        write(2, prog, strlen(prog));
    }
    write(2, msg, strlen(msg));
    char ch = (char)c;
    write(2, &ch, 1);
    write(2, "\n", 1);
}

int getopt(int argc, char *const argv[], const char *optstring)
{
    if (optind == 0) {
        /* GNU-style reset. */
        optind = 1;
        optpos = 0;
    }

    if (optind >= argc || argv[optind] == 0) return -1;

    /* Non-option / end-of-options markers. */
    if (argv[optind][0] != '-') {
        if (optstring[0] == '-') {
            optarg = argv[optind++];
            return 1;
        }
        return -1;
    }
    if (argv[optind][1] == 0)              return -1;   /* "-" alone */
    if (argv[optind][1] == '-' &&
        argv[optind][2] == 0) {
        ++optind;
        return -1;                          /* "--" terminator */
    }

    if (optpos == 0) optpos = 1;
    int c = (unsigned char)argv[optind][optpos];
    char *optchar_pos = argv[optind] + optpos;
    ++optpos;

    /* If we've consumed the whole token, advance argv. */
    if (argv[optind][optpos] == 0) {
        ++optind;
        optpos = 0;
    }

    /* Strip leading mode-flag chars from optstring before searching. */
    const char *p = optstring;
    if (*p == '-' || *p == '+') ++p;

    /* Locate `c` in optstring. */
    const char *spec = 0;
    for (const char *q = p; *q; ++q) {
        if (*q == c && c != ':') { spec = q; break; }
    }
    if (!spec) {
        optopt = c;
        if (optstring[0] != ':' && opterr) {
            emit_unknown(argv[0], ": unrecognized option: -", c);
        }
        (void)optchar_pos;
        return '?';
    }

    /* Argument required? */
    if (spec[1] == ':') {
        optarg = 0;
        if (spec[2] != ':' || optpos) {
            /* Required argument (or optional argument that's joined
             * in the same argv element). */
            optarg = argv[optind++];
            if (optpos) optarg += optpos;
            optpos = 0;
        }
        if (optind > argc) {
            optopt = c;
            if (optstring[0] == ':') return ':';
            if (opterr) {
                emit_unknown(argv[0], ": option requires an argument: -", c);
            }
            return '?';
        }
    }
    return c;
}
