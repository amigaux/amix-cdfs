## 2026-10-27 — docs/amix-module-port.md: the plan for the AMIX target as a loadable filesystem module (d1c46a4)

Under amix-dlm C4-6 v2: _load → vfs_register, init with (row, index); class boot on a disk-rooted box, root on the CD medium,
pinned; packages cdfs-k30/-k46 (cdfs30/cdfs46); the kernel-import inventory from `nm -u` on libcdfskern.o (33 names) against
DDI 1.2: sdqueue, sdopen, vn_rele, copyinstr, getudev and eight fs_* helpers are missing; the ABI text carries no vfs/vnode/
vattr/uio/dirent/statvfs layouts; the kit's sd.h says SDCARDS 2 against C4-1's 4; the kit's -traditional cannot compile the
ANSI code (object built with the repo's flags, passed as EXTRA). Ten open questions for dlm. Docs only.

## 2026-10-13 — CI: upstream's "ODFileSystem CI" workflow runs only in reinauer/ODFileSystem (748b0d6)

ci-build.yml gets the same job-level repository guard as aminet-release, release and coverity, so none of
upstream's workflows run in jusii/amix-cdfs. Known item for any upstream PR: the shared-core changes made
for the Amix kernel grow the AmigaOS 3 ROM-profile handler to 32772 bytes against its 32768 limit.

## 2026-10-13 — public repository (github.com/jusii/amix-cdfs), full history kept (84b120c)

README fork notice, NOTICE with Amiga Corporation's wording, LICENSE BSD-2-Clause with a second copyright
line; upstream's release, Aminet and Coverity workflows run only in reinauer/ODFileSystem; the internal
rebase plan replaced by docs/upstream-sync.md. Clean amix-kernel build unchanged
(libcdfskern.o md5 694a6e147e6ac418a9f604a16b0636cb).

## 2026-10-11 — the Km_OsizeWanted mount wedge: block cache 32, never sleep on oversize kmem (6b16d68, af55633)

The kernel block cache shrinks from 128 to 32 entries (slab 264,204 -> 67,596 B, 65 -> 17 contiguous pages) and
oversize kmem requests use KM_NOSLEEP, so a fragmented sptmap gives ENOMEM instead of an unkillable sleep at PZERO.
libcdfskern.o md5 694a6e147e6ac418a9f604a16b0636cb. Audited 2026-10-11 (disassembly diff, ASan, fault injection): no
memory-safety change; one 2 KiB leak on a failed meta grow under NOSLEEP remains (upstream core, post-release).

## 2026-10-03 — reads spanning 4 KiB are never short before EOF (db71e18..1403437)

Regression test for a short-read report from file transfers off a cdfs mount: a read spanning a 4 KiB boundary
returns the full count until EOF, clearing cdfs as the cause.

## 2026-10-02 — the AMIX kernel object carries no 68060-trapping 64-bit multiplies (b383d14)

include/odfs/arith.h provides odfs_udiv32/umod32/sdiv32/smod32/udivsz/umul64, switched on by ODFS_NO_MUL64,
which only the ODFS_PLATFORM_AMIX_KERNEL arm sets; used at every site that produced a mulul/mulsl candidate
(cdda, hfs, hfsplus, udf, namefix, cache_block, the amix-kernel glue). scan060 on libcdfskern.o 40 -> 0
(md5 673cd9f4 -> 83955331); AmigaOS, host and AMIX userspace builds unchanged; new unit test test_arith;
host 116/116, golden 51/51, malformed 36/36.

## 2026-09-01 — docs: the AMIX port documented at the front door (063dc1d, 72ba3d1, 6514e3b, b6ca4fa, 4f3bb51, 81c8721, 446ec77, 1c4779a)

README gains a "Building for AMIX (SVR4/m68k)" chapter (two build arms, the GCC 2.7.2.3
toolchain and its override variables, the vfssw[]/relink boundary that lies outside this tree,
the mount -F cdfs syntax, known limits) and repo-relative links. platform/amix-kernel/README.md is
new: file inventory, the ld -r build, the four hard contracts (0fcf308 no VFS_INIT, 6446103
self-locating cdfsinit, fa9db1a+31e8c3b page-aligned DMA, 9360542 no stack sdcom), the mount errno
table. platform/amix/README.md gets the errno-diagnosis table e0ef11a made possible.
docs/architecture.md: three frontends, five media backends behind odfs_media_ops_t. docs/testing.md:
the five AMIX targets are validated out of band, not in CI; host baseline 22 suites / 111 tests.
An upstream merge plan was re-measured against upstream/main 441d765: 39/36 commits, 28 hunks in
10 files; core/cache_block.c is a silent auto-merge over fa9db1a and needs a check by hand.

## 2026-09-01 — build: neutral AMIX cross-prefix default, amix-kernel phony (e9abd8d)

AMIX_CROSS_PREFIX ?= /opt/amix-cross replaces a home-directory default; the amix-kernel target is
declared .PHONY.

## 2026-08-13 — amix-kernel: mount failures stop laundering into EIO (e0ef11a)

An unregistered SCSI card now returns ENXIO (with a cmn_err naming the card),
a non-CD target ENOTTY, bad-format/unsupported discs EINVAL; EIO means genuine
I/O. New codes ODFS_ERR_NO_DEVICE / ODFS_ERR_NOT_OPTICAL; AmigaOS paths untouched.
Machine-level verified (objdump jump-table diff); all suites green. Motivated by
a SCSI card-numbering investigation on a machine with more than 64 MB: three
investigations chased ghosts because ENXIO, BAD_FORMAT and IO all read as errno 5.

## 2026-08-07 — amix-kernel: the sdcom moves off the per-process kernel stack (9360542)

A kernel panic seen during hardware validation was this defect, not an upstream-sync regression: the
SCSI command block lived on the per-process kernel stack and was DMA'd after the frame was gone.
