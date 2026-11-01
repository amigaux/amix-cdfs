/*
 * dirbench.c — host model of `ls -l DIR` on the AMIX in-kernel cdfs:
 * getdents in batches of B entries (as cdfs_readdir serves them), then a
 * lookup of every name in strcmp order (ls sorts, then stats each name).
 * Counts Rock Ridge record parses (rr_parse), block-cache reads and
 * misses, media sector reads (one SCSI command each on the AMIX kernel),
 * and host bytes held. Not part of `make`; build from the repository root:
 *
 *   cc -std=gnu11 -O2 -w -I include -I backends -o /tmp/dirbench \
 *      tools/amix-dirbench/dirbench.c $(find core backends -name '*.c') \
 *      platform/host/file_media.c \
 *      -Wl,--wrap=rr_parse,--wrap=malloc,--wrap=calloc,--wrap=free
 *   /tmp/dirbench image.iso big [batch [mode [meta_kib [shuffle]]]]
 *
 * mode 0: resume NULL with the skip in the callback (cdfs before 1.1.0-3);
 * mode 1: resume = ODFS_META_COOKIE_TAG | skip (cdfs_readdir now).
 * meta_kib sets the metadata budget; shuffle != 0 looks names up in a
 * shuffled order. Exit status is nonzero on a leak or a failed lookup.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "odfs/api.h"
#include "odfs/cache_meta.h"
#include "rock_ridge/rock_ridge.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

odfs_err_t odfs_media_open_image(const char *path, odfs_media_t *out);

static long rr_parses;
void __real_rr_parse(const uint8_t *, size_t, int, rr_info_t *, struct odfs_cache *);
void __wrap_rr_parse(const uint8_t *sua, size_t len, int skip, rr_info_t *info,
                     struct odfs_cache *cache)
{
    rr_parses++;
    __real_rr_parse(sua, len, skip, info, cache);
}

/* live-bytes tracking (host sizes) */
void *__real_malloc(size_t); void *__real_calloc(size_t, size_t); void __real_free(void *);
#define NT 65536u
static struct { void *p; size_t sz; } tab[NT];
static long live, peak;
static unsigned slot(const void *p) { return (unsigned)(((uintptr_t)p >> 4) % NT); }
static void add(void *p, size_t sz)
{
    unsigned h;
    if (!p) return;
    for (h = slot(p); tab[h].p; h = (h + 1) % NT);
    tab[h].p = p; tab[h].sz = sz; live += (long)sz;
    if (live > peak) peak = live;
}
static void del(void *p)
{
    unsigned h, j;
    if (!p) return;
    for (h = slot(p); tab[h].p && tab[h].p != p; h = (h + 1) % NT);
    if (tab[h].p != p) return;
    live -= (long)tab[h].sz; tab[h].p = NULL;
    for (j = (h + 1) % NT; tab[j].p; j = (j + 1) % NT) {
        void *q = tab[j].p; size_t s = tab[j].sz; unsigned k;
        tab[j].p = NULL;
        for (k = slot(q); tab[k].p; k = (k + 1) % NT);
        tab[k].p = q; tab[k].sz = s;
    }
}
void *__wrap_malloc(size_t n) { void *p = __real_malloc(n); add(p, n); return p; }
void *__wrap_calloc(size_t a, size_t b) { void *p = __real_calloc(a, b); add(p, a * b); return p; }
void __wrap_free(void *p) { del(p); __real_free(p); }

/* media wrapper: count sector reads */
static long media_reads;
static odfs_media_ops_t wops;
static const odfs_media_ops_t *rops;
static odfs_err_t w_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    media_reads += count;
    return rops->read_sectors(ctx, lba, count, buf);
}

/* the cdfs_readdir model */
typedef struct { uint32_t skip, seen, emitted, batch; char (*names)[256]; uint32_t n; int full; } rd_t;
static odfs_err_t rd_cb(const odfs_node_t *e, void *ctx)
{
    rd_t *c = ctx;
    if (c->seen < c->skip) { c->seen++; return ODFS_OK; }
    if (c->emitted >= c->batch) { c->full = 1; return ODFS_ERR_EOF; }
    snprintf(c->names[c->n++], 256, "%s", e->name);
    c->seen++; c->emitted++;
    return ODFS_OK;
}
static int cmp(const void *a, const void *b) { return strcmp(a, b); }

int main(int argc, char **argv)
{
    odfs_mount_opts_t o; odfs_media_t m; odfs_mount_t *M; odfs_node_t dir;
    uint32_t batch = argc > 3 ? (uint32_t)atoi(argv[3]) : 18, i, calls = 0;
    int mode = argc > 4 ? atoi(argv[4]) : 0;
    long held_mount, rr0, miss0, rd0, med0, found = 0;
    static char names[70000][256];
    rd_t c;
    const odfs_cache_stats_t *st;

    odfs_mount_opts_default(&o); o.cache_blocks = 32;          /* as cdfs_mount */
    if (argc > 5) o.meta_cache_kib = (uint32_t)atoi(argv[5]);
    M = __real_calloc(1, sizeof(*M));
    if (odfs_media_open_image(argv[1], &m) != ODFS_OK) return 2;
    rops = m.ops; wops = *m.ops; wops.read_sectors = w_read; m.ops = &wops;
    if (odfs_mount(&m, &o, NULL, M) != ODFS_OK) return 2;
    held_mount = live;
    if (odfs_lookup(M, &M->root, argv[2], &dir) != ODFS_OK) return 3;

    st = odfs_cache_get_stats(&M->cache);
    rr0 = rr_parses; miss0 = st->misses; rd0 = st->reads; med0 = media_reads; peak = live;
    memset(&c, 0, sizeof(c)); c.names = names; c.batch = batch;
    for (;;) {                                 /* getdents until EOF */
        uint32_t cookie;
        odfs_err_t e;
        c.full = 0; c.emitted = 0;
        if (mode == 0) {
            c.skip = c.n; c.seen = 0;
            e = odfs_readdir(M, &dir, rd_cb, &c, NULL);
        } else {
            c.skip = c.seen = c.n;
            cookie = c.n ? (ODFS_META_COOKIE_TAG | c.n) : 0;
            e = odfs_readdir(M, &dir, rd_cb, &c, &cookie);
        }
        calls++;
        if (!c.full && (e == ODFS_OK || e == ODFS_ERR_EOF)) break;
        if (c.full) continue;
        printf("readdir error %d\n", (int)e); return 4;
    }
    printf("%s %s batch=%u mode=%d entries=%u getdents=%u\n", argv[1], argv[2],
           batch, mode, c.n, calls);
    printf("  readdir: rr_parses=%ld cache_reads=%ld cache_misses=%ld media_sectors=%ld\n",
           rr_parses - rr0, (long)(st->reads - rd0), (long)(st->misses - miss0),
           media_reads - med0);
    rr0 = rr_parses; miss0 = st->misses; rd0 = st->reads; med0 = media_reads;
    qsort(names, c.n, sizeof(names[0]), cmp);
    if (argc > 6 && atoi(argv[6])) {          /* lookups in a shuffled order */
        static char tmp[256];
        srand(1);
        for (i = c.n; i > 1; i--) {
            uint32_t j = (uint32_t)rand() % i;
            memcpy(tmp, names[i - 1], 256); memcpy(names[i - 1], names[j], 256);
            memcpy(names[j], tmp, 256);
        }
    }
    for (i = 0; i < c.n; i++) {
        odfs_node_t n;
        if (odfs_lookup(M, &dir, names[i], &n) == ODFS_OK &&
            strcmp(n.name, names[i]) == 0)
            found++;
    }
    printf("  lookups: found=%ld/%u rr_parses=%ld cache_reads=%ld cache_misses=%ld media_sectors=%ld\n",
           found, c.n, rr_parses - rr0, (long)(st->reads - rd0),
           (long)(st->misses - miss0), media_reads - med0);
    printf("  host bytes: held after mount=%ld held after ls -l=%ld peak during=%ld meta.total=%u budget=%u\n",
           held_mount, live, peak, (unsigned)M->meta.total, (unsigned)M->meta.budget);
    odfs_unmount(M);
    printf("  after unmount live=%ld\n", live);
    return live != 0 || found != (long)c.n;
}
