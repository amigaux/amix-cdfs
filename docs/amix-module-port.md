# AMIX port: `cdfs` as a loadable filesystem module (plan)

Status: built. The module is `platform/amix-kernel/module/`, documented in
[`platform/amix-kernel/README.md`](../platform/amix-kernel/README.md#the-loadable-module--make--c-module),
which supersedes this page where they differ. amix-dlm answered §7: the
thirteen names are DDI minor 2 (1); the vfs/vnode layouts are not in the 1.2
ABI text yet, so running waits on the cut that carries them (2); boot class on
disk, root class by promotion of the same object (3, 4); `fs=` for every
`vfs_register` caller (5); `MOD_MISC_WRAPPER` (6); the prebuilt `EXTRA` (7);
the kit's `sd.h` has `SDCARDS 4` (8); the completion runs at level 4 or below
(9); `init` gets the row and its index (10). Since C4-6 v5 (kabi 1.4) the
module is demand class instead: the module core gives the `cdfs` type a stub
`vfssw` row, and the first `mount -F cdfs` loads it, which answers question 3
the other way (§3). The text below is the plan as written. The static object
is still built and linked as before.
This page says how the same sources become a loadable module under the Amix
module system (amix-dlm), and what that system has to provide first. It is part
of the AMIX target and does not touch the portable core.

References of the form `C4-6 §3` point at amix-dlm's contracts
(`docs/contracts/`), `30 §6` at its spec (`docs/spec/`), and `ddi.list`/`abi-*.txt`
at its ABI kit (`abi/k30/`, `abi/k46/`). Version numbers are the ones this plan
was written against: C4-6 v2, C3-5 v8, C3-6 v4, kabi 1.2 (still open).

## 1. How the AMIX kernel target links today

- **Output:** `make amix-kernel` gives one `ld -r` object,
  `build/amix-kernel/libcdfskern.o` (`Makefile:763-816`), with every kernel
  name left undefined for the whole-kernel link
  (`platform/amix-kernel/README.md`, "Building").
- **The kernel side** ([`README.md:437-455`](../README.md)): the object goes in
  as `fs/cdfs/exp` (listed in `OPT_SUBEXP`), and `master.d/filesys.c` gets the
  static row `{ "cdfs", cdfsinit, 0, 0 }`. A filesystem has no `master.d`
  driver entry and no `driver.conf` row; the `vfssw` row is the only hook. The
  fstype name is `cdfs`. On the 68040/68060 line the port's own patch gives
  `cdfs` the unused RFS row instead (C4-6 §2 says this stops for the module).
- **Init:** `cdfsinit()` (`cdfs_vfsops.c:494-521`) takes no arguments. It
  latches a pseudo-major from `getudev()` (`:505-510`), then searches
  `vfssw[0..nfstype)` for `"cdfs"`, installs `&cdfs_vfsops` there and records
  the index as `cdfs_fstype` (`:512-519`).
- **Vectors:** `cdfs_vfsops`, 8 slots in order (`cdfs_vfsops.c:525-535`);
  `cdfs_vnodeops`, 37 slots in order (`cdfs_vnodeops.c:502-541`). Both are
  K&R `int (*)()` arrays, so the `VFS_*`/`VOP_*` macros are the ABI.
- **I/O path:** the mount names a `(card,unit)` pair. `amix_kern_media.c`
  calls `sdopen(card)` (`:479`), then builds a heap `struct sdcom`, raises
  to `sdspl()`, calls `sdqueue()` and sleeps until the completion callback
  calls `wakeup` (`:234-238`, `:146-152`). No `bdevsw`, no `strategy`, no
  `struct buf`, no `iodone`, and no page cache: `getpage`/`putpage`/`map` are
  `fs_nosys` and every vnode is `VNOMAP` (`cdfs_node.c:197`).

## 2. The module shape

One source, as C3-6 asks. The module glue sits behind `-DCDFS_MODULE`, and
the static object stays buildable.

- **Wrapper:** a new `platform/amix-kernel/cdfs_mod.c` (K&R, built by the
  kit) with `MOD_MISC_WRAPPER(cdfs, cdfs_load, cdfs_unload, "...")`. A
  filesystem has no `cdevsw` row. Question 6 asks dlm to confirm this
  wrapper.
- **`_load(params)`:** `return vfs_register("cdfs", cdfsinit, &cdfs_vfsops, 0);`
  (C4-6 §3; `vsw_flag` 0, as the static row has it). It does nothing else, and
  it allocates nothing.
- **`init`:** the core calls it as `vfsinit` calls a row's init, with the row
  and its index (C4-6 §3; core patch 0040, `DLM_CALL(m, init, v, nfstype)`).
  Under `CDFS_MODULE`, `cdfsinit(vswp, idx)` stores `cdfs_fstype = idx` and
  latches the pseudo-major. The `vfssw`/`nfstype` search stays in the static
  build only, so the module does not reference `vfssw` or `nfstype`, which C4-6
  §4 keeps out of the DDI tier.
- **`_unload()`:** returns 0. There is nothing to undo, because the core takes
  back the row when a load fails. The module is `unload=never`, and a
  filesystem module with `unload=allowed` is refused (C4-6 §3).
- **Load context.** `_load` makes no I/O, does not sleep and does not allocate,
  so it is also safe in the root pass (level 7, clock stopped, 30 §3).
- **What runs at mount:** nothing changes. `mount -F cdfs` resolves the row
  through `vfs_getvfssw` in the grown table, and `VFS_MOUNT` dispatches to
  `cdfs_mount`. Sleeping in the SCSI wait is ordinary process context. Kernel
  memory follows C3-6 §1.1: every cdfs kmem request is `KM_NOSLEEP` and
  checked, so memory pressure gives `ENOMEM`, never a sleep. The largest
  request at mount is the 32-block cache slab of 67 596 bytes; a directory
  build grows only up to the 96 KiB metadata budget.

### 2.1 Kernel names the AMIX target imports

**Method:** `nm -u` on `build/amix-kernel/libcdfskern.o`. Its md5
`694a6e147e6ac418a9f604a16b0636cb` matches the clean build recorded at the
last commit that touched the kernel sources (84b120c). That gives 33
undefined names. A grep of the sources gave the use sites. Macros such as
`VN_HOLD`, `VN_INIT`, `sdspl` and `spl0` (from `<sys/inline.h>`) compile
inline and leave no symbol. They are layout dependencies, covered in §4.

| Name | Used at | In DDI (`ddi.list`)? | Module plan |
|---|---|---|---|
| `kmem_alloc`, `kmem_zalloc`, `kmem_free` | `include/odfs/alloc.h:67,87,101`; `cdfs_node.c:186`; `cdfs_vfsops.c:279` | yes, 0 | as is |
| `memcpy`, `strlen`, `strcmp`, `bcopy` | core/backends; `kstring.c:78` | yes, 0 | as is (`memmove`/`memset`/`memcmp` stay module-local, `kstring.c`) |
| `cmn_err` | `amix_kern_media.c:197,481` | yes, 0 | as is |
| `sleep`, `wakeup` | `amix_kern_media.c:237,151` | yes, 0 | as is (see question 9) |
| `vtop` | `amix_kern_media.c:215` | yes, 0 | as is |
| `suser` | `cdfs_vfsops.c:203` | yes, 0 | as is |
| `uiomove` | `cdfs_vnodeops.c:95,287,456` | yes, 0 | as is |
| `__udivdi3`, `__umoddi3`, `__lshrdi3` | compiler (64-bit offsets) | yes, 0 (k46: core) | as is |
| `vfssw`, `nfstype` | `cdfs_vfsops.c:512-515` | **never** (C3-2 §3) | gone: init's index (above) |
| `strncpy` | `cdfs_vfsops.c:423,426`; `cdfs_node.c:206` | no | **module-local** in `kstring.c` |
| `vfs_devsearch` | `cdfs_vfsops.c:274` (duplicate-drive guard) | no | **module-local**: only `cdfs` vfs carry the `cdfs` pseudo-major, so a list of mounted `(card,unit)` pairs is the same test |
| `sdqueue` | `amix_kern_media.c:235` | **no** (planned for stage 4 in C3-2 §3; C4-6 §4 relies on it) | **gap → DDI** |
| `sdopen` | `amix_kern_media.c:479` | **no** | **gap → DDI** |
| `vn_rele` (from `VN_RELE`) | `cdfs_vfsops.c:377,382` | no | **gap → DDI** |
| `copyinstr` | `cdfs_vfsops.c:259` (mount spec) | no (`copyin` is) | **gap → DDI** (a byte-wise `copyin` loop is the fallback) |
| `getudev` | `cdfs_vfsops.c:506` | no | **gap → DDI** (a registry pseudo-major is the fallback) |
| `fs_nosys`, `fs_setfl`, `fs_frlock`, `fs_poll`, `fs_pathconf`, `fs_cmp`, `fs_rwlock`, `fs_rwunlock` | `cdfs_vnodeops.c:507-541` | no | **gap → DDI**, as one "filesystem helpers" group (`fs_frlock` and `fs_pathconf` carry stock semantics that a module copy would drift from) |

Not used at all: the page cache (`pvn_*`, `segmap`, `page_*`), `bdevsw`, `strategy`, `iodone`,
`struct buf`, credentials beyond passing `cred_t *` to `suser`, `rootvfs`, `u.`.

## 3. Class

Superseded: the module is now demand class with `fs=cdfs`, loaded at the
first `mount -F cdfs` through the stub row of C4-6 v5 §5 (kabi minor 4), on
disk-rooted boxes and on the install media alike. `disable` in
`modreg.local.d` still turns it off. The plan as written follows.

- **On a disk-rooted box: boot class** (C4-6 §5, 30 §6). The stock mount path
  has no load-by-type hook, so a demand-class `cdfs` would never be loaded.
  The module loads at the first `sysinit` entry (30 §3), before any `rc`
  script or `/etc/vfstab` mount. So an install or data CD can be mounted
  from then on, and nothing in the base system mounts a CD earlier. Cost: a
  69 KB image is always resident. `match=always`, because no match key
  describes "a CD drive is present". A box without a drive can turn the
  module off with `disable` in `modreg.local.d` (C3-5 §1).
- **On the CD medium: root class.** The root is the RAM-disk miniroot, and
  `cdfs` mounts the disc as the package source (50 §4). `cdfs` is never the
  root filesystem: `cdfs_mountroot` returns `EROFS` (`cdfs_vfsops.c:462-465`).
- **Pinned:** `unload=never` (C4-6 §1).

Recommendation: boot class by default on disk-rooted boxes, as C4-6 v2 §5
says; demand class would leave `cdfs` never loaded (see question 3).

## 4. k30 and k46: the vfs/vnode ABI

- The `abi=` fingerprint (`fea18182…`, the SHA-256 of `abi-k30.1.2.txt` and
  `abi-k46.1.2.txt`, identical, 33 lines) covers DDI data names (lines 1–10)
  and the `offs` of `cdevsw`/`bdevsw` rows, `mod_drv_data`, `qinit`,
  `streamtab` and `mod_str_data` (lines 11–33). **None of the structures
  `cdfs` touches is in it:** `struct vfs` (`vfs_op`, `vfs_data`, `vfs_dev`,
  `vfs_fsid`, `vfs_flag`, `vfs_bsize`, `vfs_fstype`, `vfs_bcount`), `struct
  vnode` (`v_count`, `v_flag`, `v_type`, `v_vfsp`, `v_op`, `v_data`, and all
  that `VN_INIT` writes), `struct vfsops` (8 + `vfs_filler[8]`), `struct
  vnodeops` (37 + `vop_filler[32]`), `struct vattr`, `struct uio`
  (`uio_offset`, `uio_resid`), `struct dirent`, `struct statvfs`, `struct
  mounta`, `struct fid`, and `struct sdcom`. As the ABI text stands, nothing
  checks these layouts, so a module that uses them is not protected by it.
- **What makes it safe in practice:** both lines run the stock 2.1 layouts.
  The k30 kernel is relinked from the stock objects, and the k46 kernel is the
  stock binary with patches. The two built kits' headers differ only in
  `sys/immu.h` and `sys/param.h` (4 KiB page geometry on k46; checked with
  `diff -rq` on the kits' `sysroot/` and `stock/`). `vfs.h`, `vnode.h`,
  `uio.h`, `dirent.h`, `statvfs.h`, `mount.h` and the alien `sd.h` are the
  same on both lines. The static `cdfs` object, built once from these headers,
  has run on both lines.
- **Per-line difference that reaches `cdfs`:** `NBPP` is 2048 on k30 and 4096
  on k46 (`amix_kern_media.c:196,285`). Either value is correct. The plan pins
  the DMA-page check at 2048 so that both objects keep the path that was proven
  on hardware.
- **Recommendation:** add `offs` lines for the structures above (sizes and
  the fields used) to the 1.2 text before 1.2 closes (question 2).

## 5. Package

Following C3-5 §1.1, one package per line:

| | k30 | k46 |
|---|---|---|
| catalog name | `cdfs-k30` | `cdfs-k46` |
| pkginst (≤ 9) | `cdfs30` | `cdfs46` |
| short name (≤ 7) | `cdfs` | `cdfs` |
| version | `platform/amix-kernel/module/VERSION`, `1.0.0-1` as planned (`1.1.0-1` since the module is demand class), both lines | |
| files | `/etc/conf/mod.d/cdfs`, `/etc/conf/modreg.d/cdfs` (comments only) | |
| `.moddata` | `name=cdfs class=boot match=always fs=cdfs unload=never desc=…`; no `majors`, no `param` | |

`CATEGORY system`, with no device nodes and no scripts. The `mount -F cdfs`
helper (`platform/amix/mount.c`) is userland and stays in the existing `cdfs`
userland package. A box needs both packages to mount a CD.

**Build:** `platform/amix-kernel/module/` will hold `Makefile`, `keys`
(Master generated from them, as in amix-modules' ramdisk) and `VERSION`. The
kit compiles with `-traditional`, which the portable ANSI sources cannot take.
So the Makefile builds `libcdfskern-<line>.o` with this repo's kernel flags
against the kit's `sysroot/` and `stock/` headers for that line, using the
kit's pinned compiler, passes it as `EXTRA`, and compiles only `cdfs_mod.c`
as `OBJS`. Then come `mkmod`, `modfix` and `modcheck.py`, and the 68060
disassembly scan on both objects (no `-m68040`: the code stays 68020 with
`ODFS_NO_MUL64`).

## 6. Test plan

1. **Host:** `make check` unchanged. Both modules build, and `modcheck.py`
   passes. A gate checks that `nm -u` of each module is a subset of
   `ddi.list` at 1.2. The 68060 scan finds zero candidates.
2. **Emulation first** (an A4091 with an emulated SCSI CD, a kabi 1.2 kernel
   per line):
   - the boot-class load at the first `sysinit` (`modadmin` lists `cdfs`);
   - `mount -F cdfs` of the repository's `.iso` fixtures: ISO 9660 + Rock
     Ridge, Joliet, HFS, HFS+, UDF;
   - the first stage-4 media's CD-boot discs as data discs (k30
     `8c9c5003…`, k46 `8062eba5…`);
   - file contents compared with host `imgcat`, plus readdir, lookup and
     readlink;
   - a mount/umount loop (the earlier remount-panic class);
   - the refusals: `EBUSY` for a second mount, `ENXIO`/`ENOTTY` for a wrong
     address, `EEXIST` against a kernel that still has the static row, and
     safe mode (no boot class, so `mount` fails cleanly with `EINVAL`);
   - on the CD medium, root class: boot RAM-disk-rooted and mount the disc as
     the package source.
   - Keep the medium's hazard: probe once, mount once (50 §4, "a second cdfs
     mount on an absent unit panics").
3. **Real hardware:** the same rows on the A4091 and z3660scsi controllers,
   one line at a time, after emulation passes.

## 7. Open questions for amix-dlm

1. **DDI 1.2 names.** `sdqueue` and `sdopen` (C3-2 §3 lists `sdqueue` for
   stage 4, and C4-6 §4 assumes it), `vn_rele`, `copyinstr`, `getudev`, and the
   eight `fs_*` helpers. *Recommend:* all of them at minor 2. Without the first
   three the module cannot exist. `strncpy` and `vfs_devsearch` become
   module-local, so they need no request.
2. **ABI text.** The vfs/vnode/vattr/uio/dirent/statvfs/mounta/fid/sdcom
   layouts are not fingerprinted. *Recommend:* add `offs` lines for them to the
   1.2 text before it closes.
3. **Boot by default?** C4-6 v2 §5 says boot class. Demand class cannot
   work for a filesystem: the stock mount path has no load-by-type hook, so
   nothing would ever load `cdfs`. *Recommend:* boot class on disk-rooted
   boxes, with `disable` for boxes without a drive.
4. **One object, two classes.** The class is in `.moddata` (C3-5 §3), but
   `cdfs` is boot on disk and root on the CD medium. *Recommend:* let
   `kbootset` place a `class=boot` module in the root class on a generation
   that asks for it, since `_load` is already root-context safe. The
   alternative is two objects that differ only in `.moddata`.
5. **`fs=` for boot class.** C3-1 §3 marks `fs` "required: root-class
   filesystems". *Recommend:* any module that calls `vfs_register`.
6. **Wrapper.** *Recommend:* `MOD_MISC_WRAPPER` / `$modtype misc` for a
   filesystem module. Confirm or name another.
7. **Prebuilt `EXTRA` object.** The portable code is ANSI and the kit
   compiles `-traditional`. *Recommend:* accept an `EXTRA` object built by the
   pinned compiler against the kit's headers (the `cc=` pin is unchanged).
8. **`SDCARDS`.** The kit's `sd.h` says 2, and C4-1 says 4 on both lines, so
   `cdfs` would refuse cards 2–3 (`amix_kern_media.c:460`). *Recommend:* the
   kit's `sd.h` carries C4-1's 4 before `cdfs` is built.
9. **Completion level.** The `sdcom` callback calls `wakeup`. C3-6 §1.3
   forbids DDI calls at level 6. *Recommend:* C4-1 states that controller
   modules call `cp->intr` at level 4 or below.
10. **Init arguments.** The static kernel's boot walk was observed to call
    `vsw_init` with no usable arguments (`platform/amix-kernel/README.md`,
    contract 2). *Recommend:* `ddi.md` states that `vfs_register` passes the
    row and its index, as patch 0040 does, so `cdfs` can rely on the index.
