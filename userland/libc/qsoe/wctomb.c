/*
 * wctomb.c — wide-char to multibyte, ASCII-only.
 *
 * QSOE 0.7..0.99 carry no locale or multibyte machinery (those musl
 * subtrees are entirely excluded).  The only caller that matters
 * today is vfprintf's %lc/%S path; it just needs a function that
 * encodes ASCII (and reports an error for anything beyond).
 *
 * POSIX: with s=NULL, return 0 (no state-dependent encoding).
 *        with valid s, store the encoded bytes and return their
 *        count; -1 on error.
 *
 * wchar_t is `int` on RISC-V64 musl (wide enough for any UCS-4
 * codepoint); we accept that signature without pulling in the
 * <stddef.h>/<wchar.h> machinery.
 */

int wctomb(char *s, int wc);
int wctomb(char *s, int wc)
{
    if (!s) return 0;
    if (wc < 0 || wc > 0x7F) return -1;
    s[0] = (char)wc;
    return 1;
}
