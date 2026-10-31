## 2026-10-31 — cdfs is a demand-class module, loaded at the first mount; kabi 1.4 (3f84217, f2fc390)

The module's keys say class=demand with fs=cdfs, so on a kabi 1.4 kernel the first `mount -F cdfs` loads it through
the core's stub vfssw row; it is no longer loaded at boot, and `disable` in modreg.local.d still turns it off. Both
objects carry kabi 1.4 (k30 ecfe560c, k46 2af81d8c, 98 935 bytes each, 69 704-byte image); module version 1.1.0-1.
The module Makefile's default kit is ../amix/kernel/dlm. It needs kbootset -31 or later.

## 2026-10-27 — cdfs module: the k46 object passes modcheck with PAGESHIFT_OK (3418217)

The seven CD-sector divisions (offset / 2048) are waived per function in the module Makefile (amix-dlm 8fc87de's modcheck -w):
`ok (k46.1.2, boot, 69704 bytes); 7 page-geometry shift(s) waived in …`; k30 ok as before; objects unchanged (3038f6fd /
2808fcd9); harness and host test green. Kits must be minted from amix-dlm 8fc87de or later.

## 2026-10-31 — docs: public text cleanup (95bbb71)

docs/amix-module-port.md states why cdfs is a boot-class module (C4-6 v2 §5) without plan pointers; the local tool
ignores move from .gitignore to .git/info/exclude. Docs only; the objects do not change.

## 2026-10-27 — cdfs as a boot-class loadable filesystem module, kabi 1.2 (f8875bb, e1bfa50, d66beb9, ef96470)

-DCDFS_MODULE from the same sources (static libcdfskern.o byte-identical, 694a6e14). init takes (row, index) from vfs_register;
a module-local mounted-drive table replaces vfs_devsearch; strncpy local; DMA page pinned at 2048 both lines. .moddata
name=cdfs class=boot match=always fs=cdfs unload=never; MOD_MISC_WRAPPER; _load = vfs_register("cdfs", cdfsinit,
&cdfs_vfsops, 0). New platform/amix-kernel/module/ (Makefile, keys, Master, VERSION 1.0.0-1, modgate.py, hosttest.sh).
k30: modcheck ok (69704 B, 3038f6fd); k46: every rule passes but the page-geometry scan, which flags seven `moveq #11 + lsrl`
CD-sector divisions (offset / 2048) as page shifts — an amix-dlm checker question (2808fcd9). nm -u is DDI-only (32 names);
in-place ready; host test of the load path green; harness green. Running waits on the -79/-14 kernels (13 names + layouts).

## 2026-10-27 — docs/amix-module-port.md: the plan for the AMIX target as a loadable filesystem module (d1c46a4)

Under amix-dlm C4-6 v2: _load → vfs_register, init with (row, index); class boot on a disk-rooted box, root on the CD medium,
pinned; packages cdfs-k30/-k46 (cdfs30/cdfs46); the kernel-import inventory from `nm -u` on libcdfskern.o (33 names) against
DDI 1.2: sdqueue, sdopen, vn_rele, copyinstr, getudev and eight fs_* helpers are missing; the ABI text carries no vfs/vnode/
vattr/uio/dirent/statvfs layouts; the kit's sd.h says SDCARDS 2 against C4-1's 4; the kit's -traditional cannot compile the
ANSI code (object built with the repo's flags, passed as EXTRA). Ten open questions for amix-dlm. Docs only.

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
