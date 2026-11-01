/*
 * cache_meta.c — parsed-directory metadata cache
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A directory is cached as one allocation: header, compact entry array,
 * and a string pool with the namefix-final names. Repeat lookups scan
 * the compact entries, and listing continuations resume by entry index
 * (cookies tagged with ODFS_META_COOKIE_TAG). Untagged non-zero cookies
 * are backend byte offsets and are never answered here; a tagged cookie
 * that outlives its cached directory is answered by re-enumerating the
 * backend and skipping already-delivered entries, so both cookie schemes
 * stay valid across builds and evictions.
 */

#include "odfs/cache_meta.h"

#if ODFS_FEATURE_CACHE_META

#include "odfs/alloc.h"
#include "odfs/string.h"
#include <string.h>

#define META_NO_COMMENT 0xffffffffUL

/* ------------------------------------------------------------------ */
/* LRU bookkeeping                                                     */
/* ------------------------------------------------------------------ */

static void meta_unlink(odfs_meta_cache_t *mc, odfs_meta_dir_t *d)
{
    if (d->lru_prev)
        d->lru_prev->lru_next = d->lru_next;
    else
        mc->lru_head = d->lru_next;
    if (d->lru_next)
        d->lru_next->lru_prev = d->lru_prev;
    else
        mc->lru_tail = d->lru_prev;
    d->lru_prev = NULL;
    d->lru_next = NULL;
}

static void meta_push_head(odfs_meta_cache_t *mc, odfs_meta_dir_t *d)
{
    d->lru_prev = NULL;
    d->lru_next = mc->lru_head;
    if (mc->lru_head)
        mc->lru_head->lru_prev = d;
    else
        mc->lru_tail = d;
    mc->lru_head = d;
}

static void meta_evict(odfs_meta_cache_t *mc, odfs_meta_dir_t *d)
{
    meta_unlink(mc, d);
    mc->total -= d->bytes;
    mc->stats.evictions++;
    odfs_free(d);
}

void odfs_meta_cache_init(odfs_meta_cache_t *mc, uint32_t budget_bytes)
{
    memset(mc, 0, sizeof(*mc));
    mc->budget = budget_bytes;
}

void odfs_meta_cache_flush(odfs_meta_cache_t *mc)
{
    while (mc->lru_tail)
        meta_evict(mc, mc->lru_tail);
    memset(mc->neg, 0, sizeof(mc->neg));
    mc->neg_next = 0;
}

/* ------------------------------------------------------------------ */
/* failed builds                                                       */
/* ------------------------------------------------------------------ */

static odfs_meta_neg_t *meta_neg_find(odfs_meta_cache_t *mc,
                                      const odfs_node_t *dir)
{
    uint32_t i;

    for (i = 0; i < ODFS_META_NEG_SLOTS; i++) {
        odfs_meta_neg_t *n = &mc->neg[i];

        if (n->used &&
            n->dir_lba == dir->extent.lba &&
            n->dir_length == dir->extent.length &&
            n->dir_backend == (uint8_t)dir->backend)
            return n;
    }
    return NULL;
}

static void meta_neg_add(odfs_meta_cache_t *mc, const odfs_node_t *dir,
                         int clean)
{
    odfs_meta_neg_t *n = &mc->neg[mc->neg_next];

    memset(n, 0, sizeof(*n));
    n->dir_lba = dir->extent.lba;
    n->dir_length = dir->extent.length;
    n->dir_backend = (uint8_t)dir->backend;
    n->used = 1;
    n->clean = clean ? 1 : 0;
    mc->neg_next = (mc->neg_next + 1u) % ODFS_META_NEG_SLOTS;
}

void odfs_meta_cache_destroy(odfs_meta_cache_t *mc)
{
    odfs_meta_cache_flush(mc);
    memset(mc, 0, sizeof(*mc));
}

/* ------------------------------------------------------------------ */
/* lookup and node reconstruction                                      */
/* ------------------------------------------------------------------ */

static int meta_dir_cacheable(const odfs_meta_cache_t *mc,
                              const odfs_node_t *dir)
{
    if (mc->budget == 0)
        return 0;
    if (dir->kind != ODFS_NODE_DIR)
        return 0;
    if (dir->backend == ODFS_BACKEND_CDDA)
        return 0;
    if (dir->extent.length == 0)
        return 0;
    return 1;
}

static odfs_meta_dir_t *meta_find(odfs_meta_cache_t *mc,
                                  const odfs_node_t *dir)
{
    odfs_meta_dir_t *d;

    for (d = mc->lru_head; d; d = d->lru_next) {
        if (d->dir_lba == dir->extent.lba &&
            d->dir_length == dir->extent.length &&
            d->dir_backend == (uint8_t)dir->backend) {
            if (d != mc->lru_head) {
                meta_unlink(mc, d);
                meta_push_head(mc, d);
            }
            return d;
        }
    }
    return NULL;
}

static void meta_entry_to_node(const odfs_meta_dir_t *d,
                               const odfs_meta_entry_t *e,
                               odfs_node_t *out)
{
    size_t len;

    out->id = e->id;
    out->parent_id = d->dir_id;
    out->backend = (odfs_backend_type_t)e->backend;
    out->kind = (odfs_node_kind_t)e->kind;
    out->size = e->size;
    out->mtime = e->mtime;
    out->ctime = e->ctime;
    out->mode = e->mode;
    out->extent.lba = e->lba;
    out->extent.length = e->length;
    out->backend_data = NULL;

    len = strlen(d->pool + e->name_off);
    if (len >= sizeof(out->name))
        len = sizeof(out->name) - 1;
    memcpy(out->name, d->pool + e->name_off, len);
    out->name[len] = '\0';

    out->amiga_as.has_protection = e->has_protection;
    if (e->has_protection)
        memcpy(out->amiga_as.protection, e->protection, 4);
    out->amiga_as.has_comment = e->has_comment;
    if (e->has_comment) {
        len = strlen(d->pool + e->comment_off);
        if (len >= sizeof(out->amiga_as.comment))
            len = sizeof(out->amiga_as.comment) - 1;
        memcpy(out->amiga_as.comment, d->pool + e->comment_off, len);
        out->amiga_as.comment[len] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* building                                                            */
/* ------------------------------------------------------------------ */

typedef struct meta_build_ctx {
    odfs_meta_entry_t *entries;
    uint32_t           count;
    uint32_t           cap;
    char              *pool;
    uint32_t           pool_len;
    uint32_t           pool_cap;
    uint32_t           budget;
    int                failed;    /* over budget or out of memory */
    /* the INDEX form, built alongside on a readdir_at walk */
    uint32_t          *ipos;
    uint32_t          *ihash;
    uint32_t           icount;
    uint32_t           icap;
    uint32_t           ilimit;    /* most entries an INDEX can hold */
    int                ifailed;
    int                clean;     /* no entry was renamed */
} meta_build_ctx_t;

static void *meta_grow(void *old, size_t old_bytes, size_t new_bytes)
{
    void *p = odfs_malloc(new_bytes);

    if (!p)
        return NULL;
    if (old) {
        memcpy(p, old, old_bytes);
        odfs_free(old);
    }
    return p;
}

/*
 * Next capacity: double from `first`, but never past `limit`, the most
 * the budget can still hold. The caller has checked that `need` fits
 * under `limit`, so the result is always at least `need`. Without the
 * clamp a directory near the budget doubled its arrays well past it
 * before the budget check failed the build.
 */
static uint32_t meta_next_cap(uint32_t cap, uint32_t need,
                              uint32_t first, uint32_t limit)
{
    uint32_t new_cap = cap ? cap : first;

    while (new_cap < need && new_cap <= limit / 2u)
        new_cap *= 2u;
    if (new_cap < need || new_cap > limit)
        new_cap = limit;
    return new_cap;
}

/* `limit` is the largest the pool may grow to within the budget */
static uint32_t meta_pool_add(meta_build_ctx_t *bc, const char *s,
                              size_t len, uint32_t limit)
{
    uint32_t off;

    if (bc->pool_len + len > bc->pool_cap) {
        uint32_t new_cap = meta_next_cap(bc->pool_cap,
                                         bc->pool_len + (uint32_t)len,
                                         1024u, limit);
        char *grown = meta_grow(bc->pool, bc->pool_len, new_cap);

        if (!grown)
            return META_NO_COMMENT;
        bc->pool = grown;
        bc->pool_cap = new_cap;
    }
    off = bc->pool_len;
    memcpy(bc->pool + off, s, len);
    bc->pool_len += (uint32_t)len;
    return off;
}

/* add one entry to the FULL form; ODFS_ERR_NOMEM when it cannot fit */
static odfs_err_t meta_add_full(meta_build_ctx_t *bc, const odfs_node_t *entry)
{
    odfs_meta_entry_t *e;
    uint32_t off;
    size_t name_len = strlen(entry->name) + 1u;
    size_t comment_len = entry->amiga_as.has_comment ?
                         strlen(entry->amiga_as.comment) + 1u : 0;
    size_t fixed = sizeof(odfs_meta_dir_t) +
                   (size_t)(bc->count + 1u) * sizeof(*bc->entries);

    /* fail before allocating anything when this entry cannot fit */
    if (fixed > bc->budget ||
        bc->budget - fixed < (size_t)bc->pool_len + name_len + comment_len)
        return ODFS_ERR_NOMEM;

    if (bc->count >= bc->cap) {
        uint32_t limit = (uint32_t)((bc->budget - sizeof(odfs_meta_dir_t) -
                                     bc->pool_len - name_len - comment_len) /
                                    sizeof(*bc->entries));
        uint32_t new_cap = meta_next_cap(bc->cap, bc->count + 1u, 32u, limit);
        odfs_meta_entry_t *grown;

        grown = meta_grow(bc->entries,
                          (size_t)bc->count * sizeof(*bc->entries),
                          (size_t)new_cap * sizeof(*bc->entries));
        if (!grown)
            return ODFS_ERR_NOMEM;
        bc->entries = grown;
        bc->cap = new_cap;
    }

    e = &bc->entries[bc->count];
    e->id = entry->id;
    e->lba = entry->extent.lba;
    e->length = entry->extent.length;
    e->size = entry->size;
    e->mtime = entry->mtime;
    e->ctime = entry->ctime;
    e->mode = entry->mode;
    e->kind = (uint8_t)entry->kind;
    e->backend = (uint8_t)entry->backend;
    e->has_protection = entry->amiga_as.has_protection;
    if (entry->amiga_as.has_protection)
        memcpy(e->protection, entry->amiga_as.protection, 4);

    off = meta_pool_add(bc, entry->name, name_len,
                        (uint32_t)(bc->budget - fixed));
    if (off == META_NO_COMMENT)
        return ODFS_ERR_NOMEM;
    e->name_off = off;

    e->has_comment = entry->amiga_as.has_comment;
    e->comment_off = META_NO_COMMENT;
    if (entry->amiga_as.has_comment) {
        off = meta_pool_add(bc, entry->amiga_as.comment, comment_len,
                            (uint32_t)(bc->budget - fixed));
        if (off == META_NO_COMMENT)
            return ODFS_ERR_NOMEM;
        e->comment_off = off;
    }

    bc->count++;
    return ODFS_OK;
}

static odfs_err_t meta_build_cb(const odfs_node_t *entry, void *ctx)
{
    meta_build_ctx_t *bc = ctx;

    if (meta_add_full(bc, entry) != ODFS_OK) {
        bc->failed = 1;
        return ODFS_ERR_NOMEM;
    }
    return ODFS_OK;
}

/* case-folded FNV-1a, so equal hashes are necessary for strcasecmp == 0 */
static uint32_t meta_name_hash(const char *name)
{
    uint32_t h = 2166136261UL;

    while (*name) {
        h ^= odfs_ascii_tolower((unsigned char)*name++);
        h *= 16777619UL;
    }
    return h;
}

/* bytes of an INDEX of n entries, and its bucket count */
static uint32_t meta_index_buckets(uint32_t n)
{
    uint32_t nb = 8;

    while (nb < n)
        nb *= 2u;
    return nb;
}

static size_t meta_index_bytes(uint32_t n)
{
    return sizeof(odfs_meta_dir_t) + (size_t)n * 3u * sizeof(uint32_t) +
           (size_t)meta_index_buckets(n) * sizeof(uint32_t);
}

static void meta_index_drop(meta_build_ctx_t *bc)
{
    odfs_free(bc->ipos);
    odfs_free(bc->ihash);
    bc->ipos = NULL;
    bc->ihash = NULL;
    bc->ifailed = 1;
}

static odfs_err_t meta_add_index(meta_build_ctx_t *bc, const odfs_node_t *entry,
                                 const odfs_dir_pos_t *pos)
{
    if (bc->icount + 1u > bc->ilimit)
        return ODFS_ERR_NOMEM;

    if (bc->icount >= bc->icap) {
        uint32_t new_cap = meta_next_cap(bc->icap, bc->icount + 1u,
                                         64u, bc->ilimit);
        uint32_t *p = meta_grow(bc->ipos, (size_t)bc->icount * 4u,
                                (size_t)new_cap * 4u);

        if (!p)
            return ODFS_ERR_NOMEM;
        bc->ipos = p;
        p = meta_grow(bc->ihash, (size_t)bc->icount * 4u,
                      (size_t)new_cap * 4u);
        if (!p)
            return ODFS_ERR_NOMEM;
        bc->ihash = p;
        bc->icap = new_cap;
    }
    bc->ipos[bc->icount] = pos->offset;
    bc->ihash[bc->icount] = meta_name_hash(entry->name);
    bc->icount++;
    return ODFS_OK;
}

/*
 * readdir_at walk from 0: the FULL form while it fits, the INDEX form
 * alongside, and whether any name was renamed. The walk always reaches
 * the end, so `clean` covers the whole directory.
 */
static odfs_err_t meta_build_pos_cb(const odfs_node_t *entry,
                                    const odfs_dir_pos_t *pos, void *ctx)
{
    meta_build_ctx_t *bc = ctx;

    if (pos->renamed)
        bc->clean = 0;

    if (!bc->failed && meta_add_full(bc, entry) != ODFS_OK) {
        /* give the FULL form's memory back now: the INDEX may need it */
        bc->failed = 1;
        odfs_free(bc->entries);
        odfs_free(bc->pool);
        bc->entries = NULL;
        bc->pool = NULL;
    }
    if (!bc->ifailed && (!bc->clean || meta_add_index(bc, entry, pos) != ODFS_OK))
        meta_index_drop(bc);
    return ODFS_OK;
}

static odfs_meta_dir_t *meta_index_make(meta_build_ctx_t *bc,
                                        const odfs_node_t *dir)
{
    odfs_meta_dir_t *d;
    size_t total = meta_index_bytes(bc->icount);
    uint32_t *hash, *link, *head;
    uint32_t i;

    if (total > bc->budget)
        return NULL;
    d = odfs_malloc(total);
    if (!d)
        return NULL;

    memset(d, 0, sizeof(*d));
    d->dir_lba = dir->extent.lba;
    d->dir_length = dir->extent.length;
    d->dir_backend = (uint8_t)dir->backend;
    d->kind = ODFS_META_INDEX;
    d->dir_id = dir->id;
    d->count = bc->icount;
    d->bytes = (uint32_t)total;
    d->nbuckets = meta_index_buckets(bc->icount);
    d->pos = (uint32_t *)(d + 1);
    hash = d->pos + d->count;
    link = hash + d->count;
    head = link + d->count;
    if (d->count) {
        memcpy(d->pos, bc->ipos, (size_t)d->count * 4u);
        memcpy(hash, bc->ihash, (size_t)d->count * 4u);
    }
    memset(head, 0, (size_t)d->nbuckets * 4u);
    /* insert last to first, so each chain is in directory order */
    for (i = d->count; i > 0; i--) {
        uint32_t b = hash[i - 1u] & (d->nbuckets - 1u);

        link[i - 1u] = head[b];
        head[b] = i;
    }
    return d;
}

static odfs_meta_dir_t *meta_full_make(meta_build_ctx_t *bc,
                                       const odfs_node_t *dir)
{
    odfs_meta_dir_t *d;
    size_t entry_bytes = (size_t)bc->count * sizeof(odfs_meta_entry_t);
    size_t total = sizeof(*d) + entry_bytes + bc->pool_len;

    d = odfs_malloc(total);
    if (!d)
        return NULL;

    memset(d, 0, sizeof(*d));
    d->dir_lba = dir->extent.lba;
    d->dir_length = dir->extent.length;
    d->dir_backend = (uint8_t)dir->backend;
    d->kind = ODFS_META_FULL;
    d->dir_id = dir->id;
    d->count = bc->count;
    d->bytes = (uint32_t)total;
    d->entries = (odfs_meta_entry_t *)(d + 1);
    d->pool = (char *)d->entries + entry_bytes;
    if (entry_bytes)
        memcpy(d->entries, bc->entries, entry_bytes);
    if (bc->pool_len)
        memcpy(d->pool, bc->pool, bc->pool_len);
    return d;
}

/*
 * Enumerate the directory through the backend and pack the result into
 * a single allocation: the FULL form, or (readdir_at backends) the INDEX
 * form when the FULL one does not fit. Returns NULL if the directory
 * cannot be cached; *size_fail is set when the reason was the budget or
 * memory (not a backend error), and *clean_out when a readdir_at walk
 * found no renamed name, both of which the caller remembers.
 */
static odfs_meta_dir_t *meta_build(odfs_meta_cache_t *mc,
                                   const odfs_backend_ops_t *ops,
                                   void *backend_ctx,
                                   odfs_cache_t *block_cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir,
                                   int *size_fail,
                                   int *clean_out)
{
    meta_build_ctx_t bc;
    odfs_meta_dir_t *d = NULL;
    odfs_err_t err;

    *size_fail = 0;
    *clean_out = 0;
    memset(&bc, 0, sizeof(bc));
    bc.budget = mc->budget;

    if (ops->readdir_at) {
        bc.clean = 1;
        /* the most entries whose INDEX fits the budget */
        bc.ilimit = bc.budget > sizeof(odfs_meta_dir_t) ?
                    (uint32_t)((bc.budget - sizeof(odfs_meta_dir_t)) /
                               (3u * sizeof(uint32_t))) : 0;
        while (bc.ilimit > 0 && meta_index_bytes(bc.ilimit) > bc.budget)
            bc.ilimit--;
        if (bc.ilimit == 0)
            bc.ifailed = 1;
        err = ops->readdir_at(backend_ctx, block_cache, log, dir, 0,
                              meta_build_pos_cb, &bc);
        if (err != ODFS_OK)
            goto out;
        if (!bc.failed) {
            d = meta_full_make(&bc, dir);
        } else if (!bc.ifailed && bc.clean) {
            d = meta_index_make(&bc, dir);
            if (d)
                mc->stats.index_builds++;
        }
        if (!d) {
            *size_fail = 1;
            *clean_out = bc.clean;
        }
        goto out;
    }

    err = ops->readdir(backend_ctx, block_cache, log, dir,
                       meta_build_cb, &bc, NULL);
    if (bc.failed) {
        *size_fail = 1;
        goto out;
    }
    if (err != ODFS_OK)
        goto out;

    d = meta_full_make(&bc, dir);
    if (!d)
        *size_fail = 1;

out:
    odfs_free(bc.entries);
    odfs_free(bc.pool);
    odfs_free(bc.ipos);
    odfs_free(bc.ihash);
    if (d)
        mc->stats.builds++;
    else
        mc->stats.build_fails++;
    return d;
}

/*
 * Find or build the cached form of a directory. *orphan_out is set when
 * the built directory could not be kept within budget; the caller must
 * free it after use. A directory whose build failed for size or memory
 * is not built again until the cache is flushed.
 */
static odfs_meta_dir_t *meta_get(odfs_meta_cache_t *mc,
                                 const odfs_backend_ops_t *ops,
                                 void *backend_ctx,
                                 odfs_cache_t *block_cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *dir,
                                 int *orphan_out)
{
    odfs_meta_dir_t *d;
    int size_fail;
    int clean;

    *orphan_out = 0;

    d = meta_find(mc, dir);
    if (d) {
        mc->stats.hits++;
        return d;
    }
    mc->stats.misses++;

    if (meta_neg_find(mc, dir)) {
        mc->stats.neg_hits++;
        return NULL;
    }

    d = meta_build(mc, ops, backend_ctx, block_cache, log, dir,
                   &size_fail, &clean);
    if (!d) {
        if (size_fail)
            meta_neg_add(mc, dir, clean);
        return NULL;
    }

    if (d->bytes > mc->budget) {
        *orphan_out = 1;
        return d;
    }

    meta_push_head(mc, d);
    mc->total += d->bytes;
    while (mc->total > mc->budget && mc->lru_tail != d)
        meta_evict(mc, mc->lru_tail);
    return d;
}

/* ------------------------------------------------------------------ */
/* serving                                                             */
/* ------------------------------------------------------------------ */

static odfs_err_t meta_replay(const odfs_meta_dir_t *d,
                              odfs_dir_iter_fn callback,
                              void *cb_ctx,
                              uint32_t *resume_offset)
{
    uint32_t i = 0;
    odfs_err_t err;
    odfs_node_t *node;

    if (resume_offset && *resume_offset)
        i = *resume_offset & ~ODFS_META_COOKIE_TAG;

    /* Reconstructed node on the heap, reused across entries: a ~660 B node on
     * the stack would overrun the small SVR4/AMIX kernel stack in the readdir
     * serve path. */
    node = odfs_malloc(sizeof(*node));
    if (node == NULL)
        return ODFS_ERR_NOMEM;

    for (; i < d->count; i++) {
        meta_entry_to_node(d, &d->entries[i], node);
        err = callback(node, cb_ctx);
        if (err != ODFS_OK) {
            if (resume_offset)
                *resume_offset = ODFS_META_COOKIE_TAG | (i + 1u);
            odfs_free(node);
            return err;
        }
    }

    if (resume_offset)
        *resume_offset = ODFS_META_COOKIE_TAG | d->count;
    odfs_free(node);
    return ODFS_OK;
}

/*
 * Listing through readdir_at from entry index `first`, which starts at
 * backend position `start`; entries before `skip` are passed over. The
 * position of the entry the callback stopped at is reported so a hint
 * can point the next listing there. Nothing here touches a cached
 * directory once the walk has started: a walk can sleep for I/O, and the
 * cache may change meanwhile.
 */
typedef struct meta_at_ctx {
    uint32_t          index;      /* index of the next entry the walk gives */
    uint32_t          skip;       /* deliver from this index on */
    odfs_dir_iter_fn  callback;
    void             *cb_ctx;
    odfs_err_t        cb_err;
    int               stopped;
    uint32_t          stop_index; /* the entry the callback stopped at */
    uint32_t          stop_offset;
    uint32_t          stop_next;
} meta_at_ctx_t;

static odfs_err_t meta_at_cb(const odfs_node_t *entry,
                             const odfs_dir_pos_t *pos, void *ctx)
{
    meta_at_ctx_t *ac = ctx;
    odfs_err_t err;

    if (ac->index < ac->skip) {
        ac->index++;
        return ODFS_OK;
    }
    err = ac->callback(entry, ac->cb_ctx);
    if (err != ODFS_OK) {
        ac->stopped = 1;
        ac->cb_err = err;
        ac->stop_index = ac->index;
        ac->stop_offset = pos->offset;
        ac->stop_next = pos->next;
    }
    ac->index++;
    return err;
}

static odfs_err_t meta_at_list(const odfs_backend_ops_t *ops,
                               void *backend_ctx,
                               odfs_cache_t *block_cache,
                               odfs_log_state_t *log,
                               const odfs_node_t *dir,
                               uint32_t start, uint32_t first, uint32_t skip,
                               odfs_dir_iter_fn callback, void *cb_ctx,
                               uint32_t *resume_offset, meta_at_ctx_t *ac)
{
    odfs_err_t err;

    memset(ac, 0, sizeof(*ac));
    ac->index = first;
    ac->skip = skip;
    ac->callback = callback;
    ac->cb_ctx = cb_ctx;

    err = ops->readdir_at(backend_ctx, block_cache, log, dir, start,
                          meta_at_cb, ac);
    /* resume after the entry that stopped, as meta_replay does */
    if (resume_offset)
        *resume_offset = ODFS_META_COOKIE_TAG | ac->index;
    if (ac->stopped)
        return ac->cb_err;
    return err;
}

/* a listing of an INDEX directory, resuming at entry `first` */
static odfs_err_t meta_index_list(odfs_meta_dir_t *d,
                                  const odfs_backend_ops_t *ops,
                                  void *backend_ctx,
                                  odfs_cache_t *block_cache,
                                  odfs_log_state_t *log,
                                  const odfs_node_t *dir,
                                  uint32_t first,
                                  odfs_dir_iter_fn callback, void *cb_ctx,
                                  uint32_t *resume_offset)
{
    meta_at_ctx_t ac;

    if (first >= d->count) {
        if (resume_offset)
            *resume_offset = ODFS_META_COOKIE_TAG | d->count;
        return ODFS_OK;
    }
    return meta_at_list(ops, backend_ctx, block_cache, log, dir,
                        d->pos[first], first, first, callback, cb_ctx,
                        resume_offset, &ac);
}

/* a listing of a clean directory too big to index, from its hint */
static odfs_err_t meta_hint_list(odfs_meta_cache_t *mc,
                                 odfs_meta_neg_t *n,
                                 const odfs_backend_ops_t *ops,
                                 void *backend_ctx,
                                 odfs_cache_t *block_cache,
                                 odfs_log_state_t *log,
                                 const odfs_node_t *dir,
                                 uint32_t first,
                                 odfs_dir_iter_fn callback, void *cb_ctx,
                                 uint32_t *resume_offset)
{
    meta_at_ctx_t ac;
    uint32_t start = 0;
    uint32_t from = 0;
    odfs_err_t err;

    /* list_next is 0 until a listing has stopped somewhere */
    if (first != 0 && n->list_next != 0 && n->list_index == first) {
        start = n->list_offset;
        from = first;
        mc->stats.hint_scans++;
    } else if (first != 0 && n->list_next != 0 &&
               n->list_index + 1u == first) {
        start = n->list_next;
        from = first;
        mc->stats.hint_scans++;
    }

    err = meta_at_list(ops, backend_ctx, block_cache, log, dir, start,
                       from, first, callback, cb_ctx, resume_offset, &ac);

    /* the slot may have been reused while the walk slept */
    n = meta_neg_find(mc, dir);
    if (n && n->clean && ac.stopped) {
        n->list_index = ac.stop_index;
        n->list_offset = ac.stop_offset;
        n->list_next = ac.stop_next;
    }
    return err;
}

/* deliver entries after skipping the first `skip`, counting by index */
typedef struct meta_skip_ctx {
    uint32_t          skip;
    uint32_t          index;
    odfs_dir_iter_fn  callback;
    void             *cb_ctx;
    odfs_err_t        cb_err;
    int               stopped;
} meta_skip_ctx_t;

static odfs_err_t meta_skip_cb(const odfs_node_t *entry, void *ctx)
{
    meta_skip_ctx_t *sc = ctx;
    odfs_err_t err;

    if (sc->index++ < sc->skip)
        return ODFS_OK;

    err = sc->callback(entry, sc->cb_ctx);
    if (err != ODFS_OK) {
        sc->stopped = 1;
        sc->cb_err = err;
    }
    return err;
}

odfs_err_t odfs_meta_readdir(odfs_meta_cache_t *mc,
                             const odfs_backend_ops_t *ops,
                             void *backend_ctx,
                             odfs_cache_t *block_cache,
                             odfs_log_state_t *log,
                             const odfs_node_t *dir,
                             odfs_dir_iter_fn callback,
                             void *cb_ctx,
                             uint32_t *resume_offset)
{
    uint32_t resume = resume_offset ? *resume_offset : 0;
    int tagged = (resume & ODFS_META_COOKIE_TAG) != 0;
    uint32_t first = resume & ~ODFS_META_COOKIE_TAG;
    odfs_meta_dir_t *d;
    odfs_meta_neg_t *n = NULL;
    int orphan;
    odfs_err_t err;

    /* untagged non-zero cookies are backend byte offsets */
    if (resume != 0 && !tagged)
        return ODFS_ERR_UNSUPPORTED;

    if (!meta_dir_cacheable(mc, dir)) {
        if (!tagged)
            return ODFS_ERR_UNSUPPORTED;
        d = NULL;
        orphan = 0;
    } else {
        d = meta_get(mc, ops, backend_ctx, block_cache, log, dir, &orphan);
        if (!d)
            n = meta_neg_find(mc, dir);
    }

    if (d && d->kind == ODFS_META_INDEX)
        return meta_index_list(d, ops, backend_ctx, block_cache, log, dir,
                               first, callback, cb_ctx, resume_offset);
    if (d) {
        err = meta_replay(d, callback, cb_ctx, resume_offset);
        if (orphan)
            odfs_free(d);
        return err;
    }
    if (n && n->clean && ops->readdir_at)
        return meta_hint_list(mc, n, ops, backend_ctx, block_cache, log, dir,
                              first, callback, cb_ctx, resume_offset);

    if (!tagged)
        return ODFS_ERR_UNSUPPORTED;

    /*
     * A tagged continuation whose directory can no longer be cached:
     * re-enumerate and skip the entries already delivered so the
     * index-based cookie stays valid.
     */
    {
        meta_skip_ctx_t sc;

        sc.skip = first;
        sc.index = 0;
        sc.callback = callback;
        sc.cb_ctx = cb_ctx;
        sc.cb_err = ODFS_OK;
        sc.stopped = 0;

        err = ops->readdir(backend_ctx, block_cache, log, dir,
                           meta_skip_cb, &sc, NULL);
        if (resume_offset)
            *resume_offset = ODFS_META_COOKIE_TAG | sc.index;
        if (sc.stopped)
            return sc.cb_err;
        return err;
    }
}

/* lookup by readdir_at: the first entry whose name matches */
typedef struct meta_find_ctx {
    const char   *name;
    odfs_node_t  *out;
    uint32_t      limit;     /* stop at entries starting here (0: none) */
    uint32_t      next;      /* where the entry after the match starts */
    int           found;
    int           one;       /* look at one entry only */
} meta_find_ctx_t;

static odfs_err_t meta_find_cb(const odfs_node_t *entry,
                               const odfs_dir_pos_t *pos, void *ctx)
{
    meta_find_ctx_t *fc = ctx;

    if (fc->limit != 0 && pos->offset >= fc->limit)
        return ODFS_ERR_EOF;
    if (odfs_strcasecmp(entry->name, fc->name) == 0) {
        *fc->out = *entry;
        fc->next = pos->next;
        fc->found = 1;
        return ODFS_ERR_EOF;
    }
    return fc->one ? ODFS_ERR_EOF : ODFS_OK;
}

static odfs_err_t meta_find_at(const odfs_backend_ops_t *ops,
                               void *backend_ctx,
                               odfs_cache_t *block_cache,
                               odfs_log_state_t *log,
                               const odfs_node_t *dir,
                               uint32_t start, uint32_t limit, int one,
                               const char *name, odfs_node_t *out,
                               meta_find_ctx_t *fc)
{
    odfs_err_t err;

    fc->name = name;
    fc->out = out;
    fc->limit = limit;
    fc->next = 0;
    fc->found = 0;
    fc->one = one;
    err = ops->readdir_at(backend_ctx, block_cache, log, dir, start,
                          meta_find_cb, fc);
    if (err == ODFS_ERR_EOF)
        err = ODFS_OK;
    return err;
}

/*
 * INDEX lookup: hash the name, parse only the records whose hash
 * matches. The candidates are copied out first: parsing can sleep for
 * I/O, and the cached directory may be evicted meanwhile.
 */
#define META_INDEX_CANDIDATES 8

static odfs_err_t meta_index_lookup(odfs_meta_dir_t *d,
                                    const odfs_backend_ops_t *ops,
                                    void *backend_ctx,
                                    odfs_cache_t *block_cache,
                                    odfs_log_state_t *log,
                                    const odfs_node_t *dir,
                                    const char *name, odfs_node_t *out)
{
    uint32_t cand[META_INDEX_CANDIDATES];
    uint32_t ncand = 0;
    uint32_t h = meta_name_hash(name);
    const uint32_t *hash = d->pos + d->count;
    const uint32_t *link = hash + d->count;
    const uint32_t *head = link + d->count;
    uint32_t j;
    meta_find_ctx_t fc;
    odfs_err_t err;

    for (j = head[h & (d->nbuckets - 1u)]; j != 0; j = link[j - 1u]) {
        if (hash[j - 1u] != h)
            continue;
        if (ncand == META_INDEX_CANDIDATES)
            return ODFS_ERR_UNSUPPORTED;    /* pathological: let the backend scan */
        cand[ncand++] = d->pos[j - 1u];
    }

    for (j = 0; j < ncand; j++) {
        err = meta_find_at(ops, backend_ctx, block_cache, log, dir,
                           cand[j], 0, 1, name, out, &fc);
        if (err != ODFS_OK)
            return err;
        if (fc.found)
            return ODFS_OK;
    }
    return ODFS_ERR_NOT_FOUND;
}

/*
 * Lookup in a clean directory too big to index: scan from where the last
 * lookup matched to the end, then from the start up to there. Names are
 * unique there (no renames), so the first match is the match, and
 * lookups in directory order (ls -l after a listing) cost one pass.
 */
static odfs_err_t meta_hint_lookup(odfs_meta_cache_t *mc,
                                   odfs_meta_neg_t *n,
                                   const odfs_backend_ops_t *ops,
                                   void *backend_ctx,
                                   odfs_cache_t *block_cache,
                                   odfs_log_state_t *log,
                                   const odfs_node_t *dir,
                                   const char *name, odfs_node_t *out)
{
    uint32_t start = n->look_offset;
    meta_find_ctx_t fc;
    odfs_err_t err;

    if (start != 0)
        mc->stats.hint_scans++;
    err = meta_find_at(ops, backend_ctx, block_cache, log, dir,
                       start, 0, 0, name, out, &fc);
    if (err == ODFS_OK && !fc.found && start != 0)
        err = meta_find_at(ops, backend_ctx, block_cache, log, dir,
                           0, start, 0, name, out, &fc);
    if (err != ODFS_OK)
        return err;
    if (!fc.found)
        return ODFS_ERR_NOT_FOUND;

    /* the slot may have been reused while the scan slept */
    n = meta_neg_find(mc, dir);
    if (n && n->clean)
        n->look_offset = fc.next;
    return ODFS_OK;
}

odfs_err_t odfs_meta_lookup(odfs_meta_cache_t *mc,
                            const odfs_backend_ops_t *ops,
                            void *backend_ctx,
                            odfs_cache_t *block_cache,
                            odfs_log_state_t *log,
                            const odfs_node_t *dir,
                            const char *name,
                            odfs_node_t *out)
{
    odfs_meta_dir_t *d;
    odfs_meta_neg_t *n;
    int orphan;
    uint32_t i;
    odfs_err_t err = ODFS_ERR_NOT_FOUND;

    if (!meta_dir_cacheable(mc, dir))
        return ODFS_ERR_UNSUPPORTED;

    d = meta_get(mc, ops, backend_ctx, block_cache, log, dir, &orphan);
    if (!d) {
        n = meta_neg_find(mc, dir);
        if (n && n->clean && ops->readdir_at)
            return meta_hint_lookup(mc, n, ops, backend_ctx, block_cache,
                                    log, dir, name, out);
        return ODFS_ERR_UNSUPPORTED;
    }

    if (d->kind == ODFS_META_INDEX)
        return meta_index_lookup(d, ops, backend_ctx, block_cache, log, dir,
                                 name, out);

    for (i = 0; i < d->count; i++) {
        if (odfs_strcasecmp(d->pool + d->entries[i].name_off, name) == 0) {
            meta_entry_to_node(d, &d->entries[i], out);
            err = ODFS_OK;
            break;
        }
    }

    if (orphan)
        odfs_free(d);
    return err;
}

#endif /* ODFS_FEATURE_CACHE_META */
