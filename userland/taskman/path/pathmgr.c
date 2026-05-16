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
#define PATHMGR_TARGET_MAX 128   /* longest symlink target path */

typedef struct pm_node {
    struct pm_node *parent;
    struct pm_node *sibling;
    struct pm_node *child;
    tm_pathmgr_obj_t obj;
    unsigned char has_obj;
    unsigned char is_symlink;
    unsigned char name_len;
    unsigned char target_len;
    char          name[PATHMGR_NAME_MAX];
    /* Symlink target (absolute path).  Only valid when is_symlink == 1.
     * Resolved at lookup time, not at register time, so the symlink
     * follows repath()s of the target. */
    char          target[PATHMGR_TARGET_MAX];
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
    n->is_symlink = 0;
    n->name_len = (unsigned char)name_len;
    n->target_len = 0;
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

/* Inner walk: longest-prefix lookup, possibly stopping early at a
 * symlink node.  Returns the deepest matching node or 0.  When a
 * symlink stops the walk, *out_is_symlink is set to 1. */
static pm_node_t *pm_walk(const char *path,
                          const char **out_deepest_p,
                          int *out_is_symlink)
{
    pm_node_t *node = g_root;
    pm_node_t *deepest = 0;
    const char *deepest_p = path;
    const char *p = path;
    const char *comp;
    unsigned len;

    *out_is_symlink = 0;

    if (g_root->has_obj) {
        deepest = g_root;
        deepest_p = path + 1;
    }

    while (pm_next_component(&p, &comp, &len)) {
        pm_node_t *child = pm_find_child(node, comp, len);
        if (!child) break;
        node = child;
        /* Symlinks short-circuit: hand the node back so the caller
         * can re-resolve via the target.  Symlinks are leaf entries
         * (no children); a longer path under a symlink isn't valid
         * for v0.8 — would need POSIX-style realpath. */
        if (node->is_symlink) {
            deepest = node;
            deepest_p = p;
            *out_is_symlink = 1;
            break;
        }
        if (node->has_obj) {
            deepest = node;
            deepest_p = p;
        }
    }

    *out_deepest_p = deepest_p;
    return deepest;
}

int tm_pathmgr_resolve(const char *path,
                       tm_pathmgr_obj_t *out,
                       unsigned *out_consumed_bytes)
{
    if (!path || path[0] != '/' || !out || !g_root) return -EINVAL;

    const char *deepest_p = path;
    int is_symlink = 0;
    pm_node_t *deepest = pm_walk(path, &deepest_p, &is_symlink);
    if (!deepest) return -ENOENT;

    /* Symlink: re-walk via the target path.  One level only — chains
     * are not supported in v0.8 and would just reject as ENOENT. */
    if (is_symlink) {
        const char *target_p = deepest_p;        /* unused for second walk */
        int target_is_symlink = 0;
        pm_node_t *target = pm_walk(deepest->target, &target_p,
                                    &target_is_symlink);
        if (!target || target_is_symlink || !target->has_obj) {
            return -ENOENT;
        }
        *out = target->obj;
        /* For symlinks we report the link's own consumed length,
         * not the target's — the open path that the caller passed
         * was the LINK, and any sub-path under the link is on it. */
        if (out_consumed_bytes) {
            *out_consumed_bytes = (unsigned)(deepest_p - path);
        }
        return 0;
    }

    *out = deepest->obj;
    if (out_consumed_bytes) *out_consumed_bytes = (unsigned)(deepest_p - path);
    return 0;
}

int tm_pathmgr_repath(const char *path, const tm_pathmgr_obj_t *new_obj)
{
    if (!path || path[0] != '/' || !new_obj || !g_root) return -EINVAL;

    /* Walk to the EXACT node for `path` — not longest-prefix. The
     * caller wants to update a specific entry, not its parent. */
    pm_node_t *node = g_root;
    const char *p = path;
    const char *comp;
    unsigned len;
    while (pm_next_component(&p, &comp, &len)) {
        pm_node_t *child = pm_find_child(node, comp, len);
        if (!child) return -ENOENT;
        node = child;
    }
    if (!node->has_obj) return -ENOENT;
    node->obj = *new_obj;
    return 0;
}

int tm_pathmgr_symlink(const char *link_path, const char *target_path)
{
    if (!link_path || link_path[0] != '/' || !g_root) return -EINVAL;
    if (!target_path || target_path[0] != '/')        return -EINVAL;
    unsigned tlen = 0;
    while (target_path[tlen]) ++tlen;
    if (tlen == 0 || tlen >= PATHMGR_TARGET_MAX)      return -EINVAL;

    /* Grow the tree to the link path (same pattern as register). */
    pm_node_t *node = g_root;
    const char *p = link_path;
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
    if (node->has_obj || node->is_symlink) return -EEXIST;

    node->is_symlink = 1;
    node->target_len = (unsigned char)tlen;
    for (unsigned i = 0; i < tlen; ++i) node->target[i] = target_path[i];
    node->target[tlen] = 0;
    return 0;
}

int tm_pathmgr_child_at(const char *path, unsigned idx,
                        char *name_out, unsigned name_cap,
                        unsigned *out_namelen)
{
    if (!path || path[0] != '/' || !g_root) return -EINVAL;
    if (!name_out || name_cap == 0 || !out_namelen) return -EINVAL;

    /* Walk to the exact node for `path` (same shape as repath). */
    pm_node_t *node = g_root;
    const char *p = path;
    const char *comp;
    unsigned len;
    while (pm_next_component(&p, &comp, &len)) {
        pm_node_t *child = pm_find_child(node, comp, len);
        if (!child) return -EINVAL;
        node = child;
    }

    /* Skip `idx` siblings, return the next. */
    pm_node_t *c = node->child;
    unsigned i = 0;
    for (; c && i < idx; c = c->sibling, ++i) { }
    if (!c) return -ENOENT;

    unsigned nlen = c->name_len;
    if (nlen + 1 > name_cap) nlen = name_cap - 1;   /* truncate, NUL-safe */
    for (unsigned k = 0; k < nlen; ++k) name_out[k] = c->name[k];
    name_out[nlen] = 0;
    *out_namelen = nlen;
    return 0;
}
