/*
 * test_dir_index.c — a directory served from the metadata cache's INDEX
 * form, or from a position hint, must list and look up exactly as one
 * served whole from memory: same names in the same order, same nodes.
 *
 * Each fixture is mounted three times. The meta budget is set so the
 * root is cached whole (the reference), as an INDEX, or not at all (the
 * hint path for readdir_at backends, the backend for the others).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/api.h"
#include "odfs/cache_meta.h"
#include "test_harness.h"

#include <stdint.h>
#include <string.h>

#define MAX_ENTRIES 64

typedef struct listing {
    odfs_node_t e[MAX_ENTRIES];
    uint32_t    n;
} listing_t;

typedef struct batch {
    listing_t *l;
    uint32_t   got;
    uint32_t   limit;
} batch_t;

static odfs_err_t batch_cb(const odfs_node_t *entry, void *ctx)
{
    batch_t *b = ctx;

    if (b->got == b->limit)
        return ODFS_ERR_EOF;
    if (b->l->n < MAX_ENTRIES)
        b->l->e[b->l->n++] = *entry;
    b->got++;
    return ODFS_OK;
}

/* list in batches of two with index cookies, as cdfs_readdir does */
static odfs_err_t list_dir(odfs_mount_t *m, const odfs_node_t *dir,
                           listing_t *l)
{
    batch_t b;
    uint32_t next = 0;

    memset(l, 0, sizeof(*l));
    b.l = l;
    for (;;) {
        uint32_t cookie = next ? (ODFS_META_COOKIE_TAG | next) : 0;
        odfs_err_t err;

        b.got = 0;
        b.limit = 2;
        err = odfs_readdir(m, dir, batch_cb, &b, &cookie);
        next += b.got;
        if (err == ODFS_OK)
            return ODFS_OK;
        if (err != ODFS_ERR_EOF)
            return err;
    }
}

static int same_node(const odfs_node_t *a, const odfs_node_t *b)
{
    return strcmp(a->name, b->name) == 0 &&
           a->extent.lba == b->extent.lba &&
           a->extent.length == b->extent.length &&
           a->size == b->size && a->kind == b->kind &&
           a->mode == b->mode && a->backend == b->backend;
}

static int mount_with_budget(const char *image, uint32_t budget,
                             odfs_mount_t *m)
{
    odfs_media_t media;

    if (odfs_media_open_image(image, &media) != ODFS_OK)
        return -1;
    if (odfs_mount(&media, NULL, NULL, m) != ODFS_OK) {
        odfs_media_close(&media);
        return -1;
    }
    m->meta.budget = budget;
    return 0;
}

static void check_image(const char *image, int *_fail_count)
{
    static odfs_mount_t ref, m;
    static listing_t want, got;
    /* whole; an INDEX (the root has at most a dozen entries); neither */
    static const uint32_t budgets[2] = { 300, 100 };
    uint32_t b, i;

    ASSERT_EQ(mount_with_budget(image, 96u * 1024u, &ref), 0);
    ASSERT_OK(list_dir(&ref, &ref.root, &want));
    ASSERT(want.n > 0);
    ASSERT_EQ(ref.meta.stats.index_builds, 0);

    for (b = 0; b < 2; b++) {
        ASSERT_EQ(mount_with_budget(image, budgets[b], &m), 0);
        ASSERT_OK(list_dir(&m, &m.root, &got));
        ASSERT_EQ(got.n, want.n);
        for (i = 0; i < want.n; i++)
            ASSERT(same_node(&got.e[i], &want.e[i]));
        /* a second listing, then lookups backwards (the hint wraps) */
        ASSERT_OK(list_dir(&m, &m.root, &got));
        ASSERT_EQ(got.n, want.n);
        for (i = want.n; i > 0; i--) {
            odfs_node_t n;

            ASSERT_OK(odfs_lookup(&m, &m.root, want.e[i - 1].name, &n));
            ASSERT(same_node(&n, &want.e[i - 1]));
        }
        {
            odfs_node_t n;

            ASSERT_ERR(odfs_lookup(&m, &m.root, "no-such-name", &n),
                       ODFS_ERR_NOT_FOUND);
        }
        if (m.backend_ops->readdir_at && b == 0)
            ASSERT_EQ(m.meta.stats.index_builds, 1);
        odfs_unmount(&m);
    }
    odfs_unmount(&ref);
}

TEST(rock_ridge_index_and_hint_match_whole)
{
    check_image("tests/images/test_rr.iso", _fail_count);
}

TEST(plain_iso_index_and_hint_match_whole)
{
    check_image("tests/images/test_plain.iso", _fail_count);
}

TEST(rock_ridge_joliet_index_and_hint_match_whole)
{
    check_image("tests/images/test_rr_joliet.iso", _fail_count);
}

TEST(joliet_without_readdir_at_still_matches)
{
    check_image("tests/images/test_joliet.iso", _fail_count);
}

TEST_MAIN()
