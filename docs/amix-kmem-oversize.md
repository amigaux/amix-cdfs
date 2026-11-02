# cdfs and the stock kmem oversize path (`Km_OsizeWanted`)

Finding for the 2026-10-10 A4000D wedge (real 68060, kernel KD060-ZI-CD-NR,
cdfs object from `b383d14`): after about eight `mount -F cdfs 1,5 /cdrom` /
`umount` cycles, and in a second boot on only the second mount, a new
`mount -F cdfs` slept forever on WCHAN `0x081411d0` = `&Km_OsizeWanted`.

**Short version.** cdfs does not leak: every allocation is freed on umount and
on every error path, and 8 host mount/walk/umount cycles over the Amix 2.1
install CD image (r6d, 2026-10-08) end at 0 live allocations. The wedge
comes from one large request. Each mount asks kmem for a **264 204-byte
block** (the block-cache slab), which is **65 contiguous 4 KiB pages of
`sptmap`**. Stock kmem cannot always satisfy that,
and it then sleeps uninterruptibly until somebody frees an oversize block at
least that large. No other subsystem allocates blocks that large, so the sleep
never ends.

## 1. How the stock kmem handles a request over 4096 bytes

Read from the disassembly of the 68060 kernel KD060-ZI-CD-NR (`kmem_alloc` at `.text+0x41fa8`, `kmem_free` at `+0x4243a`, `sptalloc` at
`+0xa8bb6`). It matches the SVR4.0 `kma.c`:

- `size <= 256`: small pool. `size <= 4096` (`MAXABIG`): big (buddy) pool.
- `size > 4096` is **oversize**: `clicks = (size + 4095) >> 12`, then
  `sptalloc(clicks, PG_V, 0, flags & KM_NOSLEEP)`. `sptalloc` calls
  `malloc(sptmap, clicks)`, a first-fit search of the kernel-VA resource map,
  and **returns 0 at once** when no run of `clicks` contiguous free pages
  exists. It does not wait.
- On that failure with `KM_SLEEP`, `kmem_alloc` sets
  `Km_OsizeWanted = min(Km_OsizeWanted, size)` and calls
  `sleep(&Km_OsizeWanted, PZERO)`. PZERO means signals cannot interrupt it, so
  `kill -9` does nothing.
- The only wakeup is in `kmem_free` (oversize branch) and `kmem_freepool`,
  **only when the freed block is at least `Km_OsizeWanted` bytes**.

`sptmap` is small. It is the 4 MiB arena `[syssegs, kvsegmap)`, shared with
`page[]` and the u-areas (see the AMIX grimoire,
[ram-ceiling.md](https://github.com/jusii/grimoire-amix/blob/master/docs/how-it-works/ram-ceiling.md)).
Measured on the 128 MB KD060 kernel line, about **500–549 pages are free**. Other users take pieces of it and give them back: u-areas,
kmem pool pages (4 KiB / 16 KiB, released only when a pool is fully idle),
physio maps for raw I/O, and STREAMS. Because the map is first-fit, short
long-lived allocations end up inside the holes that freed long runs leave
behind. The free total stays healthy while the **largest contiguous run**
shrinks.

`availrmem` is not the limit: the box reported 129 MB available.

## 2. Every allocation cdfs makes from kmem, with sizes

The kernel arm of `odfs_malloc` adds an 8-byte header.
`odfs_malloc_aligned(s, a)` asks for `s + a + 4 + 8`. The struct sizes below
come from `m68k-cbm-sysv4-gcc` 2.7.2.3 with the kernel flags:
`struct cdfs_vfs` 6648, `odfs_node_t` 664, `struct cdfsnode` 744,
`struct cdfs_symlink` 1176, `odfs_cache_entry_t` 24, `odfs_meta_entry_t` 72,
`odfs_meta_dir_t` 40, `odfs_toc_t` 1196.

Pages are 4 KiB on KD060 and 2 KiB on the stock 030 line. A `—` in the pages
column means the request is not oversize (it comes from the small or big
pool).

| Allocation | kmem request (bytes) | Pages (4 KiB / 2 KiB) | Lifetime | Freed |
|---|---|---|---|---|
| block-cache slab, 128 × 2048, sector-aligned (`cache_block.c`) | **264 204** | **65 / 130** | mount | `odfs_cache_destroy` ← `odfs_unmount`, and on every `odfs_mount` failure |
| read-ahead `stream_buf`, 8 × 2048, aligned (gone since module 1.1.0-5, below) | 18 444 | 5 / 10 | mount | same |
| `struct cdfs_vfs` (`kmem_zalloc`, no header) | 6 648 | 2 / 4 | mount | `cdfs_unmount`, and all three `cdfs_mount` failure exits |
| parsed-dir blob (`cache_meta.c`), 48 + 72·n + names | r6d: largest 4 899 (62 entries); total 15 541 over 20 dirs | 2 / 3 for the largest | mount (LRU) | `odfs_meta_cache_destroy`; capped by the 96 KiB budget |
| cache entries / buckets / chain | 3 080 / 1 036 / 520 | — | mount | `odfs_cache_destroy` |
| INQUIRY and READ CAPACITY scratch (`NBPP`, aligned) | 4 108 each | 2 / 3 each | mount-time, transient | each exit of the probe |
| PVD scan buffer (`session.c`), aligned | 4 108 | 2 / 3 | mount-time, transient | yes |
| `cdfs_read` staging `CDFS_RDBUF` | 8 192 | 2 / 4 | each `read(2)` | yes |
| read bounce (unaligned target), aligned | 4 108 | 2 / 3 | each unaligned chunk | yes |
| `meta_build` growth arrays (doubling) | up to about 2× the final blob | 2+ | transient | yes, at `out:` |
| sdcom request, node scratch, readdir scratch (530), readlink (512), `cdfsnode` (744), symlink (1176), TOC (1196) | ≤ 4 096 | — | per op / per vnode | yes |

The path table is never loaded: `iso9660.c` only records
`path_table_size`. The only part that depends on the disc is the parsed-dir
blob, and the 96 KiB meta budget bounds it.

**Largest single `kmem_alloc` for the r6d disc: 264 204 bytes = 65 contiguous
4 KiB `sptmap` pages** (130 on a 2 KiB-page kernel). It is the same for every
disc, because it is `ODFS_BLOCK_CACHE_SIZE` (128) × 2048 plus alignment.

**Held while mounted (4 KiB pages):**

- Right after mount: 72 oversize pages (slab 65 + `stream_buf` 5 + `cvfs` 2).
- After a full tree walk of r6d: 74 (+2 for the 62-entry directory).

That is about 296 KiB, or 13–15 % of the free `sptmap`.

**Left behind per mount/umount cycle: 0 bytes.**

## 3. Evidence that nothing leaks

- **Code reading.** `cdfs_mount` frees `cvfs` on the `odfs_kern_media_open_scsi`,
  `odfs_mount` and `cdfs_getnode` failures, and closes the media on the
  second and third. `odfs_mount` destroys both caches on its two failure exits.
  `cdfs_unmount` releases the root vnode (`vop_inactive` → `kmem_free` of the
  node and its symlink side data). It then calls `odfs_unmount`, which unmounts
  the backends, destroys the meta and block caches, and closes the media (the
  `amix_kern_ctx_t`). Finally it frees `cvfs`. Every `kmem_free` / `odfs_free`
  passes the size the allocation used: the header round-trips it.
- **Measurement.** `tools/amix-kmprof/kmprof.c` wraps
  `malloc`/`calloc`/`free` around the portable core, maps each request onto
  the kernel-arm kmem cost, and runs mount → full walk (readdir, lookup, 8 KiB
  reads of all 161 files / 301 177 347 bytes) → umount over
  the Amix 2.1 install CD image (r6d, 2026-10-08). All 8 cycles end with
  `live=0 bytes=0 oversize_live_pages=0`. There are 26 oversize requests per
  cycle, the peak is 76 oversize pages, and the largest request is 264 204
  bytes.
- **On the box.** The second boot wedged on the **second** cdfs mount since boot.
  One earlier mount holds and returns at most 72–74 pages, far below the
  500-odd free pages. Running out in that boot needs `sptmap` to fragment, not
  cdfs to leak. Between the two mounts that boot ran 64 KB raw writes (physio
  maps), GSIO ioctls and two SRSTs. cdfs shows the problem first because it
  makes the largest contiguous request in the system.

## 4. Fix options

None of these is a missing free, so none is the one obvious local change.
They are listed by how much they help:

1. **Shrink the kernel block cache.** Do it in `cdfs_mount` (platform only):
   build `odfs_mount_opts_default()` opts, set `cache_blocks`, and pass them to
   `odfs_mount`.
   - At 32 blocks: slab = 32·2048 + 2048 + 12 = 67 596 bytes = **17** pages,
     and read-ahead is kept (it needs ≥ 32 blocks).
   - At 16 blocks: 34 828 bytes = **9** pages, and read-ahead is off.

   File reads hardly use the block cache: `odfs_cache_read_bytes` bypasses it
   for aligned runs of whole sectors.
2. **Fail instead of hanging.** In the kernel arm of `odfs/alloc.h`, pass
   `KM_NOSLEEP` when `total > 4096`. Do the same for the `cvfs`
   `kmem_zalloc` in `cdfs_mount`. Every caller already turns NULL into
   `ODFS_ERR_NOMEM`, which `mount(2)` returns as `ENOMEM`. Caveat:
   `odfs_cache_init` then falls back to one 4 108-byte aligned buffer per entry,
   which is 2 pages each (64 pages at 32 blocks, 256 at 128). Combine this with
   option 1, or keep the fallback out of the kernel.
3. **Fewer oversize requests per I/O.** Fewer of them means less fragmentation:
   - `CDFS_RDBUF` 8192 → 4096 is a big-pool block. Buddy blocks are aligned to
     their size, so direct DMA stays valid.
   - The NBPP-aligned 4 108-byte probe and bounce buffers could come from
     exact-size big-pool blocks, but `odfs_free_aligned` would then need a
     kernel-arm variant that knows the size.

The recommendation for 1.0 is **1 + 2**. Together they are about 15 lines in
`platform/amix-kernel/` plus the existing kernel arm of `odfs/alloc.h`. After
that, the worst case is an `ENOMEM` from `mount`, not a mount table wedged
until reboot.

### Applied (2026-10-10): 1 + 2

- `cdfs_mount` passes `cache_blocks = 32` (`CDFS_CACHE_BLOCKS`). The slab is
  67 596 bytes = 17 pages, and read-ahead stays on. The per-entry fallback
  stays in place: if the slab fails it costs 32 × 2 non-contiguous pages.
- The kernel arm of `odfs/alloc.h` passes `KM_NOSLEEP` when the kmem request
  is over 4096 bytes. The 6 648-byte `cvfs` `kmem_zalloc` does the same.
- Harness, r6d, 8 cycles at 32 blocks (`kmprof image.iso 8 32`): every cycle
  ends at `live=0`, the largest request is 67 596 bytes, and the peak is 26
  oversize pages (22 after mount, 24 after the walk). At 128 blocks it was
  264 204 bytes and 76 pages.
- Option 3 is not applied. With `CDFS_RDBUF` at 4096, an aligned read sends
  2 sectors per SCSI command instead of 4. The 8 KiB staging buffer is still a
  `KM_SLEEP` request of 2 pages, held only for one `read(2)`.

### Applied later: no cdfs request sleeps

- The media now sends one sector per SCSI command, so `CDFS_RDBUF` is 4096
  (a big-pool block, not oversize) and `KM_NOSLEEP`.
- Every kmem request cdfs makes is `KM_NOSLEEP` and checked: the kernel arm
  of `odfs/alloc.h`, and `cdfs_getnode`, `cdfs_readdir`, `cdfs_readlink`.
- The kernel arm has no per-sector fallback for the slab
  (`ODFS_CACHE_SLAB_FALLBACK` 0): the mount fails with `ENOMEM`.
- A namefix chunk is a 4096-byte request, not 4116 (two oversize pages).
- The directory cache grows only up to its budget, and a directory that does
  not fit is remembered, not rebuilt on every `getdents` and lookup.
- No read-ahead in the kernel arm (`ODFS_FEATURE_CACHE_STREAM` 0, module
  1.1.0-5): with one sector per SCSI command it saved no commands, so the
  18 444-byte `stream_buf` (5 / 10 pages) is no longer allocated per mount.
  `kmprof test_rr.iso 2 32` (host model, CDDA and read-ahead off): after
  mount 6 live allocations and 17 oversize pages, down from 7 and 22; peak
  19 oversize pages, down from 24; the largest request is still the
  67 596-byte slab.

## 5. Reproducer on the box

Get the symbol addresses from the kernel that is actually booted. On the host,
run `nm <kernel> | egrep ' (sptmap|Km_OsizeWanted)$'`. For KD060-ZI-CD-NR,
`Km_OsizeWanted` is `0x081411d0` (the WCHAN).

```sh
# what the stuck mount is asking for: 264204 (0x4080c) = the slab
echo '0x081411d0/D' | adb - /dev/kmem
# sptmap: word 0 = free slots, word 1 = mapwant, then (m_size, m_addr) pairs
# up to the first m_size of 0; the largest m_size is the largest free run
echo '<sptmap>,200/2X' | adb - /dev/kmem
```

Loop it and record the map, with a gate so the box does not wedge:

```sh
i=1
while [ $i -le 12 ]; do
  echo '<sptmap>,200/2X' | adb - /dev/kmem > /tmp/spt.$i   # before mount
  # gate: if the largest m_size in /tmp/spt.$i is < 65, STOP — mount would wedge
  mount -F cdfs 1,5 /cdrom || break
  ls -lR /cdrom > /dev/null; cat /cdrom/MANIFEST > /dev/null
  umount /cdrom
  i=`expr $i + 1`
done
```

Run the other load between iterations (64 KB raw `dd` to the IDE slice, GSIO,
SRST) and dump the map after each step. That shows which step breaks up the
large run.

Pass criteria:

- The total free pages in `sptmap` return to the same value after every
  umount. That proves cdfs does not leak on the box.
- The wedge happens exactly when the largest free run drops below 65.

With fix 1 at 32 blocks, the threshold drops to 17 pages. With fix 2, mount
fails with `ENOMEM` instead of hanging.
