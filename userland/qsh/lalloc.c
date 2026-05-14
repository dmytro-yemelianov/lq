/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Arena allocator: every allocation is hung off a `struct lalloc_common`
 * head (the Area), so an entire group of related objects can be freed
 * in one shot via afreeall() when the owning env unwinds.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

#define remalloc(p, n)          realloc_osi((p), (n))
#define ALLOC_ISUNALIGNED(p)    (((size_t)(p)) % sizeof(struct lalloc_common))

static struct lalloc_common *findptr(struct lalloc_common **, char *, Area *);

/* pre-initio() */
void
ainit(Area *ap)
{
    /* area pointer and items share struct lalloc_common */
    ap->next = NULL;
}

static struct lalloc_common *
findptr(struct lalloc_common **lpp, char *ptr, Area *ap)
{
    void *lp;

    if (ALLOC_ISUNALIGNED(ptr))
        goto fail;
    /* get address of ALLOC_ITEM from user item */
    *lpp = (lp = ptr - sizeof(ALLOC_ITEM));
    /* search for allocation item in group list */
    while (ap->next != lp)
        if ((ap = ap->next) == NULL) {
        fail:
            kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, "rogue pointer %zX", (size_t)ptr);
        }
    return (ap);
}

void *
aresize1(void *ptr, size_t len1, size_t len2, Area *ap)
{
    checkoktoadd(len1, len2);
    return (aresize(ptr, len1 + len2, ap));
}

/* pre-initio() */
void *
aresize2(void *ptr, size_t fac1, size_t fac2, Area *ap)
{
    if (notoktomul(fac1, fac2))
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_NOERRNO, Tintovfl, fac1, '*', fac2);
    return (aresize(ptr, fac1 * fac2, ap));
}

/* pre-initio() ptr=NULL */
void *
aresize(void *ptr, size_t numb, Area *ap)
{
    struct lalloc_common *lp = NULL;

    /* resizing or newly allocating? */
    if (ptr != NULL) {
        struct lalloc_common *pp;

        pp = findptr(&lp, ptr, ap);
        pp->next = lp->next;
    }

    if (notoktoadd(numb, sizeof(ALLOC_ITEM))) {
        errno = E2BIG;
    alloc_fail:
        if (!initio_done) {
            SHIKATANAI write(2, SC("qsh: out of memory early\n"));
            exit(255);
        }
        kerrf0(KWF_INTERNAL | KWF_ERR(0xFF), "can't allocate %zu data bytes", numb);
    }
    if ((lp = remalloc(lp, numb + sizeof(ALLOC_ITEM))) == NULL)
        goto alloc_fail;
    if (ALLOC_ISUNALIGNED(lp)) {
        errno = EPROTO;
        goto alloc_fail;
    }

    /* area pointer and items share struct lalloc_common */
    /*XXX C99 §6.5(6) and footnote 72 may dislike this? */
    lp->next = ap->next;
    ap->next = lp;
    /* return user item address */
    return ((char *)lp + sizeof(ALLOC_ITEM));
}

void
afree(void *ptr, Area *ap)
{
    if (ptr != NULL) {
        struct lalloc_common *lp, *pp;

        pp = findptr(&lp, ptr, ap);
        /* unhook */
        pp->next = lp->next;
        /* now free ALLOC_ITEM */
        free_osimalloc(lp);
    }
}

void
afreeall(Area *ap)
{
    struct lalloc_common *lp;

    /* traverse group (linked list) */
    while ((lp = ap->next) != NULL) {
        /* make next ALLOC_ITEM head of list */
        ap->next = lp->next;
        /* free old head */
        free_osimalloc(lp);
    }
}
