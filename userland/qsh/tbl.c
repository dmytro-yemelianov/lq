/*
 * qsh — QSOE shell (derived from MirBSD mksh)
 *
 * Hash-table machinery (variables, aliases, builtins, functions).
 *
 * We use a similar collision-resolution algorithm as Python 2.5.4
 * but with a slightly tweaked implementation written from scratch.
 *
 * Copyright (c) 19xx pdksh authors
 * Copyright (c) 2002-2025 Thorsten Glaser <tg@mirbsd.org>
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sh.h"

#define INIT_TBLSHIFT 3 /* initial table shift (2^3 = 8) */
#define PERTURB_SHIFT 5 /* see Python 2.5.4 Objects/dictobject.c */

static void tgrow(struct table *);
static int tnamecmp(const void *, const void *);

/* pre-initio() tp->tbls=NULL tp->tshift=INIT_TBLSHIFT-1 */
static void
tgrow(struct table *tp)
{
    size_t i, j, osize, mask, perturb;
    struct tbl *tblp, **pp;
    struct tbl **ntblp, **otblp = tp->tbls;

    if (tp->tshift > 29)
        kerrf(KWF_INTERNAL | KWF_ERR(0xFF) | KWF_ONEMSG | KWF_NOERRNO,
              "hash table size limit reached");

    /* calculate old size, new shift and new size */
    osize = (size_t)1 << (tp->tshift++);
    i = osize << 1;

    ntblp = alloc2(i, sizeof(struct tbl *), tp->areap);
    /* multiplication cannot overflow: alloc2 checked that */
    memset(ntblp, 0, i * sizeof(struct tbl *));

    /* table can get very full when reaching its size limit */
    tp->nfree = (tp->tshift == 30) ? 0x3FFF0000UL :
                                   /* but otherwise, only 75% */
                    ((i * 3) / 4);
    tp->tbls = ntblp;
    if (otblp == NULL)
        return;

    mask = i - 1;
    for (i = 0; i < osize; i++)
        if ((tblp = otblp[i]) != NULL) {
            if ((tblp->flag & DEFINED)) {
                /* search for free hash table slot */
                j = perturb = tblp->ua.hval;
                goto find_first_empty_slot;
            find_next_empty_slot:
                j = (j << 2) + j + perturb + 1;
                perturb >>= PERTURB_SHIFT;
            find_first_empty_slot:
                pp = &ntblp[j & mask];
                if (*pp != NULL)
                    goto find_next_empty_slot;
                /* found an empty hash table slot */
                *pp = tblp;
                tp->nfree--;
            } else if (!(tblp->flag & FINUSE)) {
                afree(tblp, tp->areap);
            }
        }
    afree(otblp, tp->areap);
}

/* pre-initio() initshift=0 */
void
ktinit(Area *ap, struct table *tp, kby initshift)
{
    tp->areap = ap;
    tp->tbls = NULL;
    tp->tshift = ((initshift > INIT_TBLSHIFT) ? initshift : INIT_TBLSHIFT) - 1;
    tgrow(tp);
}

/* table, name (key) to search for, hash(name), rv pointer to tbl ptr */
struct tbl *
ktscan(struct table *tp, const char *name, k32 h, struct tbl ***ppp)
{
    size_t j, perturb, mask;
    struct tbl **pp, *p;

    mask = ((size_t)1 << (tp->tshift)) - 1;
    /* search for hash table slot matching name */
    j = perturb = h;
    goto find_first_slot;
find_next_slot:
    j = (j << 2) + j + perturb + 1;
    perturb >>= PERTURB_SHIFT;
find_first_slot:
    pp = &tp->tbls[j & mask];
    if ((p = *pp) != NULL && (p->ua.hval != h || !(p->flag & DEFINED) || strcmp(p->name, name)))
        goto find_next_slot;
    /* p == NULL if not found, correct found entry otherwise */
    if (ppp)
        *ppp = pp;
    return (p);
}

/* table, name (key) to enter, hash(n) */
struct tbl *
ktenter(struct table *tp, const char *n, k32 h)
{
    struct tbl **pp, *p;
    size_t len;

Search:
    if ((p = ktscan(tp, n, h, &pp)))
        return (p);

    if (tp->nfree == 0) {
        /* too full */
        tgrow(tp);
        goto Search;
    }

    /* create new tbl entry */
    len = strlen(n) + 1U;
    p = alloc(qccFAMsz(struct tbl, name, len), tp->areap);
    p->flag = 0;
    p->type = 0;
    p->areap = tp->areap;
    p->ua.hval = h;
    p->u2.field = 0;
    p->u.array = NULL;
    memcpy(p->name, n, len);

    /* enter in tp->tbls */
    tp->nfree--;
    *pp = p;
    return (p);
}

void
ktwalk(struct tstate *ts, struct table *tp)
{
    ts->left = (size_t)1 << (tp->tshift);
    ts->next = tp->tbls;
}

struct tbl *
ktnext(struct tstate *ts)
{
    while (--ts->left >= 0) {
        struct tbl *p = *ts->next++;
        if (p != NULL && (p->flag & DEFINED))
            return (p);
    }
    return (NULL);
}

static int
tnamecmp(const void *p1, const void *p2)
{
    const struct tbl *a = *((const struct tbl *const *)p1);
    const struct tbl *b = *((const struct tbl *const *)p2);

    return (ascstrcmp(a->name, b->name));
}

struct tbl **
ktsort(struct table *tp)
{
    size_t i;
    struct tbl **p, **sp, **dp;

    /*
     * since the table is never entirely full, no need to reserve
     * additional space for the trailing NULL appended below
     */
    i = (size_t)1 << (tp->tshift);
    p = alloc2(i, sizeof(struct tbl *), ATEMP);
    sp = tp->tbls; /* source */
    dp = p;        /* dest */
    while (i--)
        if ((*dp = *sp++) != NULL && (((*dp)->flag & DEFINED) || ((*dp)->flag & ARRAY)))
            dp++;
    qsort(p, (i = dp - p), sizeof(struct tbl *), tnamecmp);
    p[i] = NULL;
    return (p);
}
