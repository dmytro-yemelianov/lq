/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 * Copyright (c) 1998 Todd C. Miller <Todd.Miller@courtesan.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

/*
 * Copy src to string dst of size siz. At most siz-1 characters
 * will be copied. Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */
#undef strlcpy
size_t
strlcpy(char *dst, const char *src, size_t siz)
{
    const char *s = src;

    if (siz == 0)
        goto traverse_src;

    /* copy as many chars as will fit */
    while (--siz && (*dst++ = *s++))
        ;

    /* not enough room in dst */
    if (siz == 0) {
        /* safe to NUL-terminate dst since we copied <= siz-1 chars */
        *dst = '\0';
    traverse_src:
        /* traverse rest of src */
        while (*s++)
            ;
    }

    /* count does not include NUL */
    return ((size_t)(s - src - 1));
}
