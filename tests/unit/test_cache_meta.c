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

TEST_MAIN()
