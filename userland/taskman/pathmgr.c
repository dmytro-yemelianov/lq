/*
 * pathmgr.c — taskman's path namespace registry (v0.5+).
 *
 * Implementation notes:
 *
 *  - Storage is a fixed pool of pm_node_t; nodes are bump-allocated.
 *    Sufficient for early-system server registrations (a few dozen);
 *    a freelist + symbol table arrive when v0.6+ filesystems can
 *    add/remove leaves at runtime.
 *
 *  - Names are stored inline in a small per-node buffer. Long
 *    components (>= PATHMGR_NAME_MAX) are rejected. Real filesystems
 *    will live below registered prefixes — the prefix itself stays
 *    short.
 *
 *  - The root node has no name; its child chain holds the first
 *    component of every registered path.
 */

#include "pathmgr.h"

#define PATHMGR_NODES     64
#define PATHMGR_NAME_MAX  30

typedef struct pm_node {
    struct pm_node *parent;
    struct pm_node *sibling;
    struct pm_node *child;
    tm_pathmgr_obj_t obj;
    unsigned char has_obj;
    unsigned char name_len;
    char          name[PATHMGR_NAME_MAX];
} pm_node_t;

static pm_node_t g_pool[PATHMGR_NODES];
static int       g_pool_used;
static pm_node_t *g_root;

static pm_node_t *pm_alloc(const char *name, unsigned name_len, pm_node_t *parent)
{
    if (g_pool_used >= PATHMGR_NODES) return 0;
    if (name_len > PATHMGR_NAME_MAX)  return 0;
    pm_node_t *n = &g_pool[g_pool_used++];
    n->parent = parent;
    n->sibling = 0;
    n->child = 0;
    n->has_obj = 0;
    n->name_len = (unsigned char)name_len;
    for (unsigned i = 0; i < name_len; ++i) n->name[i] = name[i];
    return n;
}

void tm_pathmgr_init(void)
{
    g_pool_used = 0;
    g_root = pm_alloc("", 0, 0);
    /* g_root is allowed to fail-silently only in tests; in production
     * the boot path traps if pathmgr can't get its root. */
}

/* Find a direct child of `parent` whose name matches [comp, comp+len). */
static pm_node_t *pm_find_child(pm_node_t *parent, const char *comp, unsigned len)
{
    for (pm_node_t *c = parent->child; c; c = c->sibling) {
        if (c->name_len != len) continue;
        unsigned i;
        for (i = 0; i < len; ++i) if (c->name[i] != comp[i]) break;
        if (i == len) return c;
    }
    return 0;
}

static pm_node_t *pm_add_child(pm_node_t *parent, const char *comp, unsigned len)
{
    pm_node_t *n = pm_alloc(comp, len, parent);
    if (!n) return 0;
    n->sibling = parent->child;
    parent->child = n;
    return n;
}

/* Step over a single path component starting at *p. Sets *out_comp and
 * *out_len to the component (excluding any leading slashes); advances
 * *p past it. Returns 1 if a component was found, 0 if end-of-path. */
static int pm_next_component(const char **p, const char **out_comp, unsigned *out_len)
{
    while (**p == '/') (*p)++;
    if (**p == 0) return 0;
    *out_comp = *p;
    while (**p && **p != '/') (*p)++;
    *out_len = (unsigned)(*p - *out_comp);
    return 1;
}

int tm_pathmgr_register(const char *path, const tm_pathmgr_obj_t *obj)
{
    if (!path || path[0] != '/' || !obj || !g_root) return -EINVAL;

    pm_node_t *node = g_root;
    const char *p = path;
    const char *comp;
    unsigned len;
    while (pm_next_component(&p, &comp, &len)) {
        pm_node_t *child = pm_find_child(node, comp, len);
        if (!child) {
            child = pm_add_child(node, comp, len);
            if (!child) return -ENOMEM;
        }
        node = child;
    }

    if (node->has_obj) return -EINVAL;  /* already registered */
    node->obj = *obj;
    node->has_obj = 1;
    return 0;
}

int tm_pathmgr_resolve(const char *path,
                       tm_pathmgr_obj_t *out,
                       unsigned *out_consumed_bytes)
{
    if (!path || path[0] != '/' || !out || !g_root) return -EINVAL;

    pm_node_t *node = g_root;
    pm_node_t *deepest = 0;
    const char *deepest_p = path;
    const char *p = path;
    const char *comp;
    unsigned len;

    while (pm_next_component(&p, &comp, &len)) {
        pm_node_t *child = pm_find_child(node, comp, len);
        if (!child) break;
        node = child;
        if (node->has_obj) {
            deepest = node;
            deepest_p = p;  /* points just past this component's tail */
        }
    }

    if (!deepest) return -ENOENT;
    *out = deepest->obj;
    if (out_consumed_bytes) *out_consumed_bytes = (unsigned)(deepest_p - path);
    return 0;
}
