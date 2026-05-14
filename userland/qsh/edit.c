/*
 * qsh — QSOE shell, line editor (QSOE-original, not from mksh)
 *
 * Copyright (c) 2026 Yuri Zaporozhets <yuriz@qrv-systems.net>
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Implemented with Claude Code.
 *
 * Minimal in-house line editor.  Replaces mksh's 5800-line emacs+vi
 * monster with the smallest set of features that make an interactive
 * shell pleasant:
 *
 *   - cursor: ^A/^E/^B/^F  (home / end / back / forward)
 *   - delete: ^H, BS, ^D-on-empty(EOF), DEL, ^W (kill word)
 *   - kill:   ^U (line),  ^K (to end)
 *   - history: ^P, CursorUp / ^N, CursorDown
 *   - search:  ^R  — incremental backward search through history
 *   - other:   ^L (redraw),  TAB inserts a literal tab (completion will
 *              be added in a later step — see doc/md/qsh_port_plan.md)
 *
 * Reads from tty_fd, writes via shf_write to a buffered shf wrapping
 * tty_fd.  History access goes through histrap.c's histpos() /
 * histnum() / histsave() — exactly the same API the upstream emacs
 * handler used.
 *
 * Two pieces of state survive across x_read() calls: nothing, by
 * design.  Each call sets up the tty, runs one line, restores the
 * tty, returns.  The shell stack is the source of truth.
 */

#include "sh.h"

/* -----------------------------------------------------------------
 * Local state — alive only for the duration of one x_read() call.
 * ----------------------------------------------------------------- */

#define LINEMAX     LINE        /* max chars in one input line */

static char     *xbuf;          /* buffer the caller gave us */
static char     *xend;          /* xbuf + LINEMAX - 2 (room for \n\0) */
static char     *xcp;           /* current cursor position in xbuf */
static char     *xep;           /* one past last char in xbuf */
static int       xprompt_cols;  /* visible width of the prompt */
static int       histcursor;    /* index in history[] we're currently
                                   showing; -1 means "the user's line" */
static char     *userline_save; /* user's in-progress line, saved while
                                   browsing history; freed at x_read end */

/* -----------------------------------------------------------------
 * Low-level I/O helpers.
 *
 * No termios scaffolding: the QSOE console (devc-ser8250) delivers
 * raw bytes already; there is no canonical mode or echo to disable.
 * Line discipline (echo, BS/erase, history navigation) is implemented
 * entirely below by edit.c itself.
 * ----------------------------------------------------------------- */

static inline void
out_str(const char *s)
{
    shf_write(s, strlen(s), shl_out);
}

static inline void
out_chr(int c)
{
    char ch = (char)c;
    shf_write(&ch, 1, shl_out);
}

static inline void
out_flush(void)
{
    shf_flush(shl_out);
}

static int
read_one_char(void)
{
    unsigned char c;
    ssize_t n;

    out_flush();
    do {
        n = blocking_read(tty_fd, (char *)&c, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
        return -1;
    }
    return (int)c;
}

/* -----------------------------------------------------------------
 * Display: redraw the current edit buffer.
 *
 * Rough approach — works well for lines that fit on one terminal
 * row.  Long lines wrap visually; we don't attempt to chase the
 * cursor across multiple rows.
 * ----------------------------------------------------------------- */

static void
redraw(void)
{
    /* CR, prompt, line buffer, then erase to end of line */
    out_chr('\r');
    pprompt(prompt, 0);
    shf_write(xbuf, (size_t)(xep - xbuf), shl_out);
    out_str("\033[K");                  /* CSI K — erase to EOL */
    /* move cursor back to xcp if it isn't already at xep */
    int back = (int)(xep - xcp);
    while (back-- > 0)
        out_chr('\b');
    out_flush();
}

/* -----------------------------------------------------------------
 * Buffer manipulation primitives.  All operate on (xbuf, xcp, xep)
 * and call redraw() to refresh the screen.
 * ----------------------------------------------------------------- */

static void
buf_insert(int c)
{
    bool at_end;

    if (xep >= xend)                    /* out of buffer space */
        return;
    /* fast path: appending at end-of-line — no shift, no redraw,
       just emit the character.  Eliminates per-keystroke flicker
       in the common typing case. */
    at_end = (xcp == xep);
    if (!at_end)
        memmove(xcp + 1, xcp, (size_t)(xep - xcp));
    *xcp++ = (char)c;
    xep++;
    if (at_end) {
        out_chr(c);
        out_flush();
    } else {
        redraw();
    }
}

static void
buf_backspace(void)
{
    bool at_end;

    if (xcp == xbuf)
        return;
    /* fast path: deleting at end-of-line — emit "\b \b" and skip
       the full redraw. */
    at_end = (xcp == xep);
    memmove(xcp - 1, xcp, (size_t)(xep - xcp));
    xcp--;
    xep--;
    if (at_end) {
        out_str("\b \b");
        out_flush();
    } else {
        redraw();
    }
}

static void
buf_delete(void)
{
    if (xcp == xep)
        return;
    memmove(xcp, xcp + 1, (size_t)(xep - xcp - 1));
    xep--;
    redraw();
}

static void
buf_kill_to_eol(void)
{
    xep = xcp;
    redraw();
}

static void
buf_kill_line(void)
{
    xcp = xep = xbuf;
    redraw();
}

static void
buf_kill_word_back(void)
{
    char *p = xcp;
    while (p > xbuf && p[-1] == ' ')
        p--;
    while (p > xbuf && p[-1] != ' ')
        p--;
    if (p == xcp)
        return;
    memmove(p, xcp, (size_t)(xep - xcp));
    xep -= (xcp - p);
    xcp = p;
    redraw();
}

static void
cursor_left(void)
{
    if (xcp > xbuf) {
        xcp--;
        out_chr('\b');
        out_flush();
    }
}

static void
cursor_right(void)
{
    if (xcp < xep) {
        out_chr(*xcp);
        xcp++;
        out_flush();
    }
}

static void
cursor_home(void)
{
    while (xcp > xbuf) {
        xcp--;
        out_chr('\b');
    }
    out_flush();
}

static void
cursor_end(void)
{
    while (xcp < xep) {
        out_chr(*xcp);
        xcp++;
    }
    out_flush();
}

/* -----------------------------------------------------------------
 * History navigation.
 *
 * `history` is a global array of saved command strings; `histptr`
 * points one past the last entry.  We index from 0 (oldest) to
 * (histptr - history - 1) (newest).
 * ----------------------------------------------------------------- */

static int
history_count(void)
{
    if (history == NULL || histptr == NULL || histptr < history)
        return 0;
    /* histptr points at the NEWEST entry (init_histvec sets it to
     * history-1 for the empty case; each store advances it by one).
     * Number of stored entries = (histptr - history) + 1.
     * Returning just (histptr - history) leaves the newest unreachable
     * via arrow-up — that's the bug we're fixing here. */
    return (int)(histptr - history) + 1;
}

static void
load_history_at(int idx)
{
    if (idx < 0 || idx >= history_count())
        return;
    const char *s = history[idx];
    size_t len = strlen(s);
    if (len > (size_t)(xend - xbuf))
        len = (size_t)(xend - xbuf);
    memcpy(xbuf, s, len);
    xep = xbuf + len;
    xcp = xep;
    redraw();
}

static void
history_prev(void)
{
    int total = history_count();
    if (total == 0)
        return;

    if (histcursor < 0) {
        /* save the user's in-progress line */
        afree(userline_save, ATEMP);
        size_t len = (size_t)(xep - xbuf);
        userline_save = alloc(len + 1, ATEMP);
        memcpy(userline_save, xbuf, len);
        userline_save[len] = '\0';
        histcursor = total - 1;
    } else if (histcursor > 0) {
        histcursor--;
    } else {
        return;                         /* already at oldest */
    }
    load_history_at(histcursor);
}

static void
history_next(void)
{
    int total = history_count();
    if (histcursor < 0)
        return;

    histcursor++;
    if (histcursor >= total) {
        /* return to user's in-progress line */
        histcursor = -1;
        size_t len = userline_save ? strlen(userline_save) : 0;
        if (len > (size_t)(xend - xbuf))
            len = (size_t)(xend - xbuf);
        if (userline_save)
            memcpy(xbuf, userline_save, len);
        xep = xbuf + len;
        xcp = xep;
        redraw();
        return;
    }
    load_history_at(histcursor);
}

/* -----------------------------------------------------------------
 * Reverse incremental search (Ctrl-R).
 *
 * Lifts the prompt to "(reverse-i-search)`pattern': match"; each
 * character typed extends the pattern; another ^R jumps to the
 * next older match; ENTER accepts; ESC / ^G aborts.
 * ----------------------------------------------------------------- */

static int
search_history_backward(int from, const char *pat)
{
    if (history == NULL || pat[0] == '\0')
        return -1;
    for (int i = from; i >= 0; i--)
        if (strstr(history[i], pat) != NULL)
            return i;
    return -1;
}

static void
do_isearch(void)
{
    char pat[128];
    size_t patlen = 0;
    int    match = -1;
    int    last_from = history_count() - 1;

    pat[0] = '\0';

    for (;;) {
        /* Render: \r (reverse-i-search)`pat': matched-line  ESC[K  */
        out_str("\r(reverse-i-search)`");
        shf_write(pat, patlen, shl_out);
        out_str("': ");
        if (match >= 0)
            out_str(history[match]);
        out_str("\033[K");
        out_flush();

        int c = read_one_char();
        if (c < 0)
            break;
        if (c == '\r' || c == '\n') {
            /* accept the current match (or the user's pattern if no match) */
            if (match >= 0) {
                size_t len = strlen(history[match]);
                if (len > (size_t)(xend - xbuf))
                    len = (size_t)(xend - xbuf);
                memcpy(xbuf, history[match], len);
                xep = xbuf + len;
                xcp = xep;
                histcursor = match;
            }
            redraw();
            return;
        }
        if (c == '\033' || c == 0x07 /*^G*/) {
            /* abort — keep the original line */
            redraw();
            return;
        }
        if (c == 0x12 /*^R*/) {
            /* find next older match */
            if (match > 0) {
                int next = search_history_backward(match - 1, pat);
                if (next >= 0)
                    match = next;
            }
            continue;
        }
        if (c == 0x08 || c == 0x7F /*BS / DEL*/) {
            if (patlen > 0) {
                patlen--;
                pat[patlen] = '\0';
                match = search_history_backward(last_from, pat);
            }
            continue;
        }
        /* ordinary char — extend pattern */
        if (patlen + 1 < sizeof(pat)) {
            pat[patlen++] = (char)c;
            pat[patlen]   = '\0';
            match = search_history_backward(last_from, pat);
        }
    }
}

/* -----------------------------------------------------------------
 * Escape-sequence handler.  We only deal with what xterm-class
 * terminals send for the four arrow keys: CSI A/B/C/D.
 * ----------------------------------------------------------------- */

static void
handle_escape(void)
{
    int c = read_one_char();
    if (c != '[' && c != 'O') {
        /* not a CSI / SS3 — drop it (or handle Alt-foo here in future) */
        return;
    }
    c = read_one_char();
    switch (c) {
    case 'A': history_prev();   break;      /* CursorUp */
    case 'B': history_next();   break;      /* CursorDown */
    case 'C': cursor_right();   break;      /* CursorRight */
    case 'D': cursor_left();    break;      /* CursorLeft */
    case 'H': cursor_home();    break;      /* Home */
    case 'F': cursor_end();     break;      /* End */
    default:                    break;      /* ignore others */
    }
}

/* -----------------------------------------------------------------
 * Public entry points.
 * ----------------------------------------------------------------- */

void
x_init(void)
{
    /* Nothing to allocate ahead of time; per-call state is set up
       inside x_read().  devc-ser8250 delivers raw bytes; no tty
       termios fiddling needed. */
    histcursor    = -1;
    userline_save = NULL;
}

void
x_initterm(const char *term)
{
    /* No termcap lookup — we assume an xterm-compatible terminal
       (CSI cursor moves, ESC [K to clear EOL).  If a particular
       terminal needs special-casing in future, key off `term`
       here. */
    (void)term;
}

char *
x_read(char *buf)
{
    xbuf = buf;
    xend = buf + LINEMAX - 2;
    xcp  = xep = xbuf;
    histcursor = -1;
    afree(userline_save, ATEMP);
    userline_save = NULL;
    xprompt_cols = pprompt(prompt, 0);

    for (;;) {
        int c = read_one_char();
        if (c < 0) {
            /* read error / EOF on the tty */
            xep = xbuf;             /* return empty line */
            break;
        }

        switch (c) {
        case '\r':
        case '\n':
            *xep++ = '\n';
            out_chr('\n');
            out_flush();
            goto done;

        case 0x01: cursor_home();       break;  /* ^A */
        case 0x05: cursor_end();        break;  /* ^E */
        case 0x02: cursor_left();       break;  /* ^B */
        case 0x06: cursor_right();      break;  /* ^F */
        case 0x10: history_prev();      break;  /* ^P */
        case 0x0E: history_next();      break;  /* ^N */
        case 0x12: do_isearch();        break;  /* ^R */
        case 0x0B: buf_kill_to_eol();   break;  /* ^K */
        case 0x15: buf_kill_line();     break;  /* ^U */
        case 0x17: buf_kill_word_back();break;  /* ^W */
        case 0x0C: redraw();            break;  /* ^L */
        case 0x08:
        case 0x7F:
            buf_backspace();                    /* BS / DEL */
            break;
        case 0x04:                              /* ^D */
            if (xep == xbuf) {
                xep = xbuf;                     /* EOF on empty line */
                goto done;
            }
            buf_delete();
            break;
        case 0x09:                              /* TAB */
            /* TODO: tab completion (filename / command / variable) */
            buf_insert('\t');
            break;
        case 0x1B:                              /* ESC */
            handle_escape();
            break;
        default:
            if (c >= 0x20 && c < 0x7F)
                buf_insert(c);
            /* silently drop other control chars */
            break;
        }
    }

done:
    afree(userline_save, ATEMP);
    userline_save = NULL;
    return xep;
}
