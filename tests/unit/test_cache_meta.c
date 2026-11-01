/*
 * test_cache_meta.c — tests for the parsed-directory cache: the budget
 * threshold, and that a directory whose build failed is not rebuilt.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/cache_meta.h"
#include "test_harness.h"

#include <stdio.h>

/* fake backend: a directory of `entries` names "entry00000", ... */
typedef struct fake_dir {
    uint32_t entries;
    int      readdir_calls;
} fake_dir_t;

static odfs_err_t fake_readdir(void *backend_ctx, odfs_cache_t *cache,
                               odfs_log_state_t *log, const odfs_node_t *dir,
                               odfs_dir_iter_fn callback, void *cb_ctx,
                               uint32_t *resume_offset)
{
    fake_dir_t *fd = backend_ctx;
    odfs_node_t n;
    uint32_t i;

    (void)cache;
    (void)log;
    (void)resume_offset;
    fd->readdir_calls++;

    for (i = 0; i < fd->entries; i++) {
        odfs_err_t err;

        memset(&n, 0, sizeof(n));
        n.id = i + 1u;
        n.parent_id = dir->id;
        n.backend = ODFS_BACKEND_ISO9660;
        n.kind = ODFS_NODE_FILE;
        n.extent.lba = 100u + i;
        snprintf(n.name, sizeof(n.name), "entry%05u", (unsigned)i);
        err = callback(&n, cb_ctx);
        if (err != ODFS_OK)
            return err;
    }
    return ODFS_OK;
}

static const odfs_backend_ops_t fake_ops = {
    .name = "fake",
    .backend_type = ODFS_BACKEND_ISO9660,
    .readdir = fake_readdir,
};

static odfs_err_t count_cb(const odfs_node_t *entry, void *ctx)
{
    (void)entry;
    (*(uint32_t *)ctx)++;
    return ODFS_OK;
}

static void fake_dir_node(odfs_node_t *dir)
{
    memset(dir, 0, sizeof(*dir));
    dir->backend = ODFS_BACKEND_ISO9660;
    dir->kind = ODFS_NODE_DIR;
    dir->extent.lba = 20;
    dir->extent.length = 2048;
}

/* the bytes a cached directory of n "entryNNNNN" names takes */
static uint32_t dir_bytes(uint32_t n)
{
    return (uint32_t)(sizeof(odfs_meta_dir_t) +
                      n * (sizeof(odfs_meta_entry_t) + 11u));
}

TEST(small_directory_built_once)
{
    odfs_meta_cache_t mc;
    fake_dir_t fd = { 10, 0 };
    odfs_node_t dir;
    uint32_t seen = 0;

    fake_dir_node(&dir);
    odfs_meta_cache_init(&mc, 96u * 1024u);

    ASSERT_OK(odfs_meta_readdir(&mc, &fake_ops, &fd, NULL, NULL, &dir,
                                count_cb, &seen, NULL));
    ASSERT_OK(odfs_meta_readdir(&mc, &fake_ops, &fd, NULL, NULL, &dir,
                                count_cb, &seen, NULL));
    ASSERT_EQ(seen, 20);
    ASSERT_EQ(fd.readdir_calls, 1);
    ASSERT_EQ(mc.stats.builds, 1);
    odfs_meta_cache_destroy(&mc);
}

TEST(directory_exactly_at_budget_is_cached)
{
    odfs_meta_cache_t mc;
    fake_dir_t fd = { 700, 0 };
    odfs_node_t dir;
    uint32_t seen = 0;

    fake_dir_node(&dir);
    odfs_meta_cache_init(&mc, dir_bytes(700));

    ASSERT_OK(odfs_meta_readdir(&mc, &fake_ops, &fd, NULL, NULL, &dir,
                                count_cb, &seen, NULL));
    ASSERT_EQ(seen, 700);
    ASSERT_EQ(mc.stats.builds, 1);
    ASSERT_EQ(mc.total, dir_bytes(700));
    odfs_meta_cache_destroy(&mc);
}

TEST(directory_one_byte_over_budget_fails_once)
{
    odfs_meta_cache_t mc;
    fake_dir_t fd = { 700, 0 };
    odfs_node_t dir;
    odfs_node_t out;
    uint32_t seen = 0;

    fake_dir_node(&dir);
    odfs_meta_cache_init(&mc, dir_bytes(700) - 1u);

    ASSERT_ERR(odfs_meta_readdir(&mc, &fake_ops, &fd, NULL, NULL, &dir,
                                 count_cb, &seen, NULL),
               ODFS_ERR_UNSUPPORTED);
    ASSERT_EQ(fd.readdir_calls, 1);
    ASSERT_EQ(mc.stats.build_fails, 1);

    /* remembered: neither a listing nor a lookup builds it again */
    ASSERT_ERR(odfs_meta_readdir(&mc, &fake_ops, &fd, NULL, NULL, &dir,
                                 count_cb, &seen, NULL),
               ODFS_ERR_UNSUPPORTED);
    ASSERT_ERR(odfs_meta_lookup(&mc, &fake_ops, &fd, NULL, NULL, &dir,
                                "entry00001", &out),
               ODFS_ERR_UNSUPPORTED);
    ASSERT_EQ(fd.readdir_calls, 1);
    ASSERT_EQ(mc.stats.build_fails, 1);
    ASSERT_EQ(mc.stats.neg_hits, 2);
    ASSERT_EQ(seen, 0);

    /* forgotten with the cache */
    odfs_meta_cache_flush(&mc);
    ASSERT_ERR(odfs_meta_readdir(&mc, &fake_ops, &fd, NULL, NULL, &dir,
                                 count_cb, &seen, NULL),
               ODFS_ERR_UNSUPPORTED);
    ASSERT_EQ(fd.readdir_calls, 2);
    odfs_meta_cache_destroy(&mc);
}

TEST(failed_directory_does_not_block_others)
{
    odfs_meta_cache_t mc;
    fake_dir_t big = { 5000, 0 };
    fake_dir_t small = { 3, 0 };
    odfs_node_t bigdir;
    odfs_node_t smalldir;
    uint32_t seen = 0;

    fake_dir_node(&bigdir);
    fake_dir_node(&smalldir);
    smalldir.extent.lba = 21;
    odfs_meta_cache_init(&mc, 96u * 1024u);

    ASSERT_ERR(odfs_meta_readdir(&mc, &fake_ops, &big, NULL, NULL, &bigdir,
                                 count_cb, &seen, NULL),
               ODFS_ERR_UNSUPPORTED);
    ASSERT_OK(odfs_meta_readdir(&mc, &fake_ops, &small, NULL, NULL,
                                &smalldir, count_cb, &seen, NULL));
    ASSERT_EQ(seen, 3);
    ASSERT_EQ(mc.stats.builds, 1);
    ASSERT_EQ(mc.stats.build_fails, 1);
    odfs_meta_cache_destroy(&mc);
}

/* ------------------------------------------------------------------ */
/* readdir_at: the INDEX form and the position hints                   */
/* ------------------------------------------------------------------ */

/* entry i starts at position POS(i); "entryNNNNN" names; `renamed` marks one */
#define POS(i) (1000u + (uint32_t)(i) * 7u)

typedef struct fake_at {
    uint32_t entries;
    int      renamed_at;     /* -1: none */
    int      walks;          /* readdir_at calls */
    long     parsed;         /* entries the walks produced */
    int      readdir_calls;  /* plain readdir calls */
} fake_at_t;

static void fake_at_node(const odfs_node_t *dir, uint32_t i, odfs_node_t *n)
{
    memset(n, 0, sizeof(*n));
    n->id = i + 1u;
    n->parent_id = dir->id;
    n->backend = ODFS_BACKEND_ISO9660;
    n->kind = ODFS_NODE_FILE;
    n->extent.lba = 100u + i;
    n->size = i;
    snprintf(n->name, sizeof(n->name), "entry%05u", (unsigned)i);
}

static odfs_err_t fake_at_readdir(void *backend_ctx, odfs_cache_t *cache,
                                  odfs_log_state_t *log, const odfs_node_t *dir,
                                  odfs_dir_iter_fn callback, void *cb_ctx,
                                  uint32_t *resume_offset)
{
    fake_at_t *fa = backend_ctx;
    odfs_node_t n;
    uint32_t i;

    (void)cache; (void)log; (void)resume_offset;
    fa->readdir_calls++;
    for (i = 0; i < fa->entries; i++) {
        odfs_err_t err;

        fake_at_node(dir, i, &n);
        fa->parsed++;
        err = callback(&n, cb_ctx);
        if (err != ODFS_OK)
            return err;
    }
    return ODFS_OK;
}

static odfs_err_t fake_at_readdir_at(void *backend_ctx, odfs_cache_t *cache,
                                     odfs_log_state_t *log,
                                     const odfs_node_t *dir, uint32_t start,
                                     odfs_dir_pos_fn callback, void *cb_ctx)
{
    fake_at_t *fa = backend_ctx;
    odfs_node_t n;
    uint32_t i = 0;

    (void)cache; (void)log;
    fa->walks++;
    if (start != 0) {
        if (start < POS(0) || (start - POS(0)) % 7u != 0)
            return ODFS_ERR_INVAL;          /* not a position we reported */
        i = (start - POS(0)) / 7u;
    }
    for (; i < fa->entries; i++) {
        odfs_dir_pos_t pos;
        odfs_err_t err;

        fake_at_node(dir, i, &n);
        pos.offset = POS(i);
        pos.next = POS(i + 1u);
        pos.renamed = (int)i == fa->renamed_at;
        fa->parsed++;
        err = callback(&n, &pos, cb_ctx);
        if (err != ODFS_OK)
            return err;
    }
    return ODFS_OK;
}

static const odfs_backend_ops_t fake_at_ops = {
    .name = "fake_at",
    .backend_type = ODFS_BACKEND_ISO9660,
    .readdir = fake_at_readdir,
    .readdir_at = fake_at_readdir_at,
};

/* list in batches of `batch` with index cookies, as cdfs_readdir does */
typedef struct batch_ctx {
    uint32_t got;
    uint32_t limit;
    uint32_t expect;     /* index of the next entry expected */
    int      bad;
} batch_ctx_t;

static odfs_err_t batch_cb(const odfs_node_t *entry, void *ctx)
{
    batch_ctx_t *bc = ctx;
    char want[32];

    if (bc->got == bc->limit)
        return ODFS_ERR_EOF;
    snprintf(want, sizeof(want), "entry%05u", (unsigned)bc->expect);
    if (strcmp(entry->name, want) != 0)
        bc->bad = 1;
    bc->expect++;
    bc->got++;
    return ODFS_OK;
}

static int list_in_batches(odfs_meta_cache_t *mc, fake_at_t *fa,
                           const odfs_node_t *dir, uint32_t batch)
{
    batch_ctx_t bc;
    uint32_t next = 0;

    memset(&bc, 0, sizeof(bc));
    for (;;) {
        uint32_t cookie = next ? (ODFS_META_COOKIE_TAG | next) : 0;
        odfs_err_t err;

        bc.got = 0;
        bc.limit = batch;
        bc.expect = next;
        err = odfs_meta_readdir(mc, &fake_at_ops, fa, NULL, NULL, dir,
                                batch_cb, &bc, &cookie);
        if (bc.bad)
            return -1;
        next += bc.got;
        if (err == ODFS_OK)
            return (int)next;
        if (err != ODFS_ERR_EOF)
            return -2;
    }
}

TEST(index_serves_listing_and_lookups_in_one_pass)
{
    odfs_meta_cache_t mc;
    fake_at_t fa = { 3000, -1, 0, 0, 0 };
    odfs_node_t dir, out;
    uint32_t i;
    long parsed_before;

    fake_dir_node(&dir);
    odfs_meta_cache_init(&mc, 96u * 1024u);

    ASSERT_EQ(list_in_batches(&mc, &fa, &dir, 18), 3000);
    ASSERT_EQ(mc.stats.index_builds, 1);
    ASSERT_EQ(mc.lru_head->kind, ODFS_META_INDEX);
    ASSERT(mc.total <= mc.budget);
    /* one build walk plus one pass of listing, not one walk per batch */
    ASSERT(fa.parsed <= 2 * 3000 + 200);  /* + one extra per batch */
    ASSERT_EQ(fa.readdir_calls, 0);

    /* every lookup parses one record, in any order */
    parsed_before = fa.parsed;
    for (i = 0; i < 3000; i++) {
        char name[32];
        uint32_t k = (i * 1777u) % 3000u;

        snprintf(name, sizeof(name), "ENTRY%05u", (unsigned)k);
        ASSERT_OK(odfs_meta_lookup(&mc, &fake_at_ops, &fa, NULL, NULL, &dir,
                                   name, &out));
        ASSERT_EQ(out.extent.lba, 100u + k);
    }
    ASSERT_EQ(fa.parsed - parsed_before, 3000);
    ASSERT_ERR(odfs_meta_lookup(&mc, &fake_at_ops, &fa, NULL, NULL, &dir,
                                "entry99999", &out), ODFS_ERR_NOT_FOUND);
    odfs_meta_cache_destroy(&mc);
}

TEST(renamed_directory_gets_no_index_or_hint)
{
    odfs_meta_cache_t mc;
    fake_at_t fa = { 3000, 1500, 0, 0, 0 };
    odfs_node_t dir, out;
    uint32_t seen = 0;

    fake_dir_node(&dir);
    odfs_meta_cache_init(&mc, 96u * 1024u);

    ASSERT_ERR(odfs_meta_readdir(&mc, &fake_at_ops, &fa, NULL, NULL, &dir,
                                 count_cb, &seen, NULL),
               ODFS_ERR_UNSUPPORTED);
    ASSERT_EQ(mc.stats.index_builds, 0);
    ASSERT(mc.neg[0].used && !mc.neg[0].clean);
    ASSERT_ERR(odfs_meta_lookup(&mc, &fake_at_ops, &fa, NULL, NULL, &dir,
                                "entry00001", &out), ODFS_ERR_UNSUPPORTED);
    odfs_meta_cache_destroy(&mc);
}

TEST(hint_when_too_big_to_index)
{
    odfs_meta_cache_t mc;
    fake_at_t fa = { 3000, -1, 0, 0, 0 };
    odfs_node_t dir, out;
    uint32_t i;
    long parsed_before;

    fake_dir_node(&dir);
    odfs_meta_cache_init(&mc, 16u * 1024u);   /* INDEX of 3000 needs ~52 KiB */

    ASSERT_EQ(list_in_batches(&mc, &fa, &dir, 18), 3000);
    ASSERT_EQ(mc.stats.index_builds, 0);
    ASSERT(mc.neg[0].used && mc.neg[0].clean);
    /* the build walk, then each batch resumes where the last one stopped */
    ASSERT(fa.parsed <= 2 * 3000 + 18 * 200);

    /* lookups in directory order: one pass */
    parsed_before = fa.parsed;
    for (i = 0; i < 3000; i++) {
        char name[32];

        snprintf(name, sizeof(name), "entry%05u", (unsigned)i);
        ASSERT_OK(odfs_meta_lookup(&mc, &fake_at_ops, &fa, NULL, NULL, &dir,
                                   name, &out));
        ASSERT_EQ(out.extent.lba, 100u + i);
    }
    ASSERT(fa.parsed - parsed_before <= 3000 + 3000);

    /* a name before the hint is found by the wrap-around */
    ASSERT_OK(odfs_meta_lookup(&mc, &fake_at_ops, &fa, NULL, NULL, &dir,
                               "entry00010", &out));
    ASSERT_EQ(out.extent.lba, 110);
    ASSERT_OK(odfs_meta_lookup(&mc, &fake_at_ops, &fa, NULL, NULL, &dir,
                               "entry00005", &out));
    ASSERT_EQ(out.extent.lba, 105);
    ASSERT_ERR(odfs_meta_lookup(&mc, &fake_at_ops, &fa, NULL, NULL, &dir,
                                "nothere", &out), ODFS_ERR_NOT_FOUND);
    ASSERT_EQ(fa.readdir_calls, 0);
    odfs_meta_cache_destroy(&mc);
}

TEST_MAIN()
