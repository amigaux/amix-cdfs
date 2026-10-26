# AMIX (SVR4/m68k) — the in-kernel `cdfs` filesystem

This directory is the actual deliverable of the AMIX port: a read-only optical
filesystem that lives **inside the SVR4 kernel** as a real vfs/vnode
filesystem, so `mount -F cdfs 0,2 /cdrom` gives every process on the box a CD
under `/cdrom`. The portable ODFS core and backend parsers do the format work
unchanged; everything here is the SVR4 glue that makes them mountable, plus the
kernel-side SCSI transport that feeds them sectors.

The userspace half — the mount helper `mount -F cdfs` execs, and the media
backends the cross-built tools use — is next door in
[`platform/amix/`](../amix/README.md).

| File | Lines | What it is |
|---|---:|---|
| [`cdfs_vfsops.c`](cdfs_vfsops.c) | 565 | The `struct vfsops` vector: mount, unmount, root, statvfs, sync, vget, mountroot, swapvp — plus `cdfsinit()` |
| [`cdfs_vnodeops.c`](cdfs_vnodeops.c) | 543 | The 37-slot `struct vnodeops` vector: open/read/readdir/lookup/getattr/readlink/… |
| [`cdfs_node.c`](cdfs_node.c) | 241 | The per-mount vnode object cache, and the `odfs_err_t` → errno table |
| [`amix_kern_media.c`](amix_kern_media.c) | 524 | In-kernel SCSI media backend: `struct sdcom` + `sdqueue()` + sleep |
| [`kstring.c`](kstring.c) | 168 | The `memmove`/`memset`/`memcmp` the SVR4 kernel does not export (and the module form's `strncpy`) |
| [`cdfs.h`](cdfs.h) | 134 | `struct cdfs_vfs`, `struct cdfsnode`, the shared declarations |
| [`cdfs_blocks.h`](cdfs_blocks.h) | 103 | `st_blocks` arithmetic, factored out so the host test suite can cover it |
| [`compat/string.h`](compat/string.h) | 77 | `<string.h>` interceptor for the kernel build |
| [`amix_kern_media.h`](amix_kern_media.h) | 42 | The one entry point `cdfs_mount()` calls |
| [`cdfs_mod.c`](cdfs_mod.c) | 44 | Loadable module only: the module wrapper and `_load` |
| [`cdfs_modlocal.c`](cdfs_modlocal.c) | 100 | Loadable module only: its `cdfsinit(row, index)` and mounted-drive table |
| [`cdfs_mod.h`](cdfs_mod.h) | 40 | Loadable module only: the names above |
| [`module/`](module/) | | Loadable module only: its build (Makefile, keys, VERSION), gate and host test |

## Building — `make amix-kernel`

```sh
make amix-kernel                                      # -> build/amix-kernel/libcdfskern.o
make amix-kernel AMIX_CROSS_PREFIX=/path/to/amix-cross
```

The output is **one relocatable object** for a static kernel link, not an
executable — stock AMIX has no loadable filesystems. (The same sources also
build as a module for the Amix loadable-module system; see
[The loadable module](#the-loadable-module--make--c-module) below.) Three things make this build differ
from the userspace `make amix` arm:

1. **`-DODFS_PLATFORM_AMIX_KERNEL -D_KERNEL`** selects the kernel arm at each of
   the four portable seams — allocation via `kmem`, no `<stdio.h>`, the bounded
   formatter, and the fixed-width-type shim — so nothing here needs libc or a
   syscall. `_KERNEL` also turns on the `sys/*.h` kernel declarations that
   `systm.h` gates `sleep`/`wakeup`/`bcopy` behind, and that the alien `sd.h`
   ABI requires.
2. **The gcc wrapper is bypassed for the final link.** The cross `gcc` is a
   wrapper that always injects `crt1`/`crti`/`crtn` and libc, which is exactly
   wrong for a kernel object, so the objects are combined with the cross
   `ld -r` instead. That leaves every kernel and toolchain extern — `kmem_*`,
   `sdqueue`, `sleep`/`wakeup`, `spl*`, `vtop`, `memcpy`/`strlen`, `vfssw`,
   `nfstype` — undefined, to be resolved at the real kernel link.
3. **No `-lgcc`.** The three 64-bit soft-arithmetic helpers (`__udivdi3`,
   `__umoddi3`, `__lshrdi3`) that m68k has no instructions for are left
   undefined **on purpose**, so they bind to the kernel's own `libgcc` copies
   at the whole-kernel link rather than being duplicated into it. That is not a
   cosmetic choice: those helpers are the subject of the closed known-unknown
   §1 in [`docs/amix-known-unknowns.md`](../../docs/amix-known-unknowns.md), and
   a second private copy would have hidden the toolchain bug rather than
   exposing it.

`kstring.o` additionally compiles with **`-fno-builtin`**. Without it GCC
recognizes the byte loops in `memset`/`memcmp` as the very idioms they are
defining and rewrites them into self-calls — infinite recursion. This is the
same freestanding trap `platform/amiga/os4/freestanding.o` guards against.

## How it reaches a kernel — not from here

`build/amix-kernel/libcdfskern.o` on its own does nothing. Two further steps are
required, and **this repository performs neither**:

1. **A `cdfs` row in the kernel's static `vfssw[]` table**
   (`master.d/filesys.c` in the stock `/usr/sys` tree), of the shape
   `{ "cdfs", cdfsinit, 0, 0 }`, with an `extern int cdfsinit();` above it.
2. **A whole-kernel relink** with `libcdfskern.o` among the objects (installed
   as `fs/cdfs/exp` and listed in `OPT_SUBEXP` in `fs/Makefile`) and the cross
   toolchain's `libgcc.a` on the final `ld` line. That link is what resolves all
   the externs `ld -r` left open.

This is the step nobody can guess from the tree, so it is stated here rather
than implied: building `cdfs` and building it into a kernel are separate jobs.
The top-level [README](../../README.md#getting-the-kernel-filesystem-into-a-kernel)
lists the steps; the scripted integration harness this port was validated with
is not published yet.

## The four hard contracts

Each of these is a rule the SVR4 kernel enforces by crashing rather than by
diagnosing, and each was paid for once already.

### 1. Never call `VFS_INIT` from `cdfs_mount` — `0fcf308`

`mount(2)` has **already run** `VFS_INIT` on the vfs by the time a filesystem's
mount routine is called (it is inlined there, right after the `kmem_alloc`), and
it has then called `vfs_lock()`, which ORs `VFS_MLOCK` into `vfs_flag`.
`VFS_INIT` *assigns* `vfs_flag = 0`, so calling it a second time destroys that
live lock bit. On an `MS_REMOUNT` it is worse: `mount(2)` hands the filesystem
the **live** vfs, and `VFS_INIT` would zero `vfs_next` and truncate the global
`rootvfs` list. Stock `s5mount` calls `VFS_INIT` nowhere — it never writes
`vfs_op`, `vfs_next` or `vfs_nsubmounts`, and only ever ORs `vfs_flag`. `cdfs`
does the same.

The same commit fixed the mirror-image defect: `mount(2)` allocates the vfs with
`kmem_alloc`, **not** `kmem_zalloc`, and the inlined `VFS_INIT` clears only five
fields — so `vfs_dev`, `vfs_fsid` and `vfs_bcount` arrive holding whatever the
recycled heap block last contained. `vfs_devsearch()` scans `rootvfs` comparing
`vfs_dev` and `getvfs()` scans it comparing `vfs_fsid`; both hand the matched
`struct vfs *` straight to code that casts `vfs_data` to *its own* private mount
struct. A stale `vfs_dev` on a linked-in vfs is therefore a wild-pointer source
for unrelated kernel code, not a cosmetic defect.

`cdfs` has no block-special device, so it synthesizes one: `cdfsinit()` latches
a single pseudo-major from **`getudev()`** — the kernel's own convention for
device-less filesystems, the way `prinit()` and `fdinit()` do in the same
boot-time walk — and each mount carries `(card,unit)` in the 18 minor bits. The
result is stable across mount/unmount and unique per drive, which is what makes
the duplicate-device guard below possible at all.

### 2. `cdfsinit()` self-locates its `vfssw[]` slot by name — `6446103`

The kernel walks `vfssw[]` at boot calling each `vsw_init` routine. The obvious
implementation trusts a passed `(vswp, fstype)` pair — but the AMIX boot loop
invokes `(*vsw_init)()` with no such arguments, so a declared `vswp` is garbage
and writing through it corrupts an unrelated slot, leaving the real `"cdfs"`
row's `vsw_vfsops` NULL. `mount(2)` then returns EINVAL before it ever
dispatches, with nothing to say why.

So `cdfsinit()` takes no arguments (declared with empty parens, C89
"unspecified arguments", so it binds cleanly however the kernel invokes it),
walks `vfssw[0..nfstype)` itself looking for the row named `"cdfs"`, installs
`&cdfs_vfsops` there, and records the row index as the fstype.

### 3. Every DMA target is page-aligned and may not cross a page — `fa9db1a` + `31e8c3b`

`sc.addr` is a **physical** address (`vtop` of the buffer) and the HBA transfers
`sc.nbyte` bytes linearly through physical memory from it. `vtop()` translates
exactly one page, and SVR4 `kmem`/`sptalloc` memory is virtually contiguous but
**physically scattered** — so any transfer running past the end of the target's
page writes into an unrelated physical frame. On metal that wild write zeroed
the head of cached binaries: it is what was corrupting `/usr/sbin/umount` and
libc.

Two halves, and both are needed:

- `fa9db1a` (in portable `core/`) makes every buffer a media backend DMAs into
  **sector-aligned**, via `odfs_malloc_aligned`.
- `31e8c3b` (here) makes the contract **enforced rather than assumed**:
  `amix_kern_submit()` refuses — loudly, with a `cmn_err(CE_WARN)` — any request
  whose in-page offset plus length exceeds the DMA page
  (`AMIX_KERN_DMA_PAGE`, 2048), and `read_sectors()` routes
  any unaligned caller through a page-aligned bounce sector plus a `memcpy`
  rather than firing a wild DMA. The bounce is allocated per call, not per
  mount, because the submit sleep is a reschedule point and concurrent readers
  of one mount must not share one.

**On 4 KiB-page kernels the same object stays correct.** The rule is enforced at
2048 bytes (`AMIX_KERN_DMA_PAGE`) even where the 68040/68060 kernels run
4096-byte pages (`NBPP` 4096). That is stricter, never weaker: a 2048-aligned
2048-byte transfer always sits inside one 4096-byte page, so one object serves
both kernel lines. The value is a constant rather than `NBPP`, so a build
against 4 KiB-page headers (the loadable module's 68040/68060 object) keeps the
2048 path that was proven on both lines; a compile-time check refuses a page
size that is not a multiple of it. Nothing else in the read path depends on
the page size. A read returns min(request, size - offset) bytes, never fewer.
`tests/unit/test_read_span.c` checks both at a 2048 and a 4096 runtime page.

### 4. A `struct sdcom` handed to `sdqueue()` must never live on the kernel stack — `9360542`

On AMIX the kernel stack lives in the **per-process u-area window** — a fixed
virtual range that is remapped at every context switch. An interrupt-completing
HBA driver (a3091/a4091) delivers completion from its ISR in whatever process
context the interrupt happens to land in. While the submitter sleeps, another
process is on-CPU, and the driver's `cp` — a u-area-window address — resolves
into *that* process's stack: the ISR writes `cp->okay`/`cp->status` into the
victim's frames and jumps through a garbage `cp->intr`. That is the
`psw=0x2200` wild-pc KERNEL FAULT in an unrelated process, seen on the bench as
a panic inside `flopd`.

Every stock `sd` client obeys this contract with permanent sdcoms (`dd.c` uses
`ddtab[][].com`; `scsi.c`'s `gsioctl` uses a file-static). `cdfs` was the sole
violator. Each request is now a `kmem`-heap allocation — globally mapped in
every context — freed only after the completion handler has fired.

A synchronous, in-context driver (today's `z3660scsi`) never dereferences
cross-context, which is why the z3660scsi card survived what the a3091 card
died of. Do not read that as the rule being optional.

## Mount refusals, and what each errno means

`mount(2)` returns an errno and nothing else, so the distinctions
`cdfs_errno()` ([`cdfs_node.c`](cdfs_node.c)) preserves are the **only**
diagnosis a failed mount gets. `e0ef11a` exists because they were not preserved:
an unregistered card, a wire fault, a disc with no filesystem and a
512-byte-block device were one indistinguishable errno 5, and that cost three
separate investigations.

| errno | What it means | First thing to check |
|---|---|---|
| `ENXIO` | No controller registered at that card index — the address names nothing at all. `sdopen()` said so, and a `cmn_err` on the console names the card | The card number. The stock kernel unregisters the a3091 once memory exceeds 64 MB, so `1,3` may be a slot with no controller in it |
| `ENOTTY` | The target answered INQUIRY but is **not** peripheral type 0x05 — the device is there, it is just not a CD drive. Produced nowhere else in cdfs, so it is an unambiguous fingerprint | The unit number: this is a hard disk or a tape, not the drive you meant |
| `EINVAL` | Either the disc read fine and no backend recognized a filesystem on it, or the shape is one this driver does not serve (a block size other than 2048, a capacity past READ CAPACITY(10)) — or the mount spec did not parse | The disc, then the spec string |
| `ENOMEM` | `kmem` allocation failed | Kernel memory pressure |
| `EIO` | **Only** "the device did not deliver readable data" | Cable, termination, media |
| `EBUSY` | Either that drive is already mounted somewhere, or that directory already has something mounted on it (see the anti-stacking guard below) | `mount` with no arguments |
| `EROFS` | The caller did not pass `MS_RDONLY` | Use `/usr/lib/fs/cdfs/mount`, which always does |
| `EPERM` | Not the superuser. `mount(2)` itself checks no privilege — every filesystem's `VFS_MOUNT` does | — |
| `ENOTDIR` | The mount point is not a directory | — |

Two of those need their reason stated, because both look like over-strictness:

**`EBUSY` — the anti-stacking guard** (`844c2ed`). `mount(2)`'s only own busy
test is `v_vfsmountedhere` on the vnode `lookupname()` returned. But for a
directory that is *already* mounted on, `lookupname` **traverses into** the
mounted filesystem and returns its root vnode, whose `v_vfsmountedhere` is
clear. So without a guard a second `mount -F cdfs 0,2 /cdrom` does not fail: it
silently mounts on top of the first (rc 0), and `umount` — resolving its path
the same traversing way — then pops one layer per call, returns 0, and leaves
the disc still mounted underneath. "umount succeeded but the CD is still
mounted." `cdfs_mount` therefore refuses `v_count > 1` or `VROOT` on the mount
point, exactly as `prmount`, `fdmount` and `s5mount` do, and mirrors
`s5mount`'s duplicate-device guard via `vfs_devsearch()`.

**`EROFS` without `MS_RDONLY`.** `vfs_add()` runs *after* this routine returns
and rewrites `VFS_RDONLY` from the `mount(2)` flags alone — setting it if
`MS_RDONLY` was passed and **clearing** it otherwise. A `VFS_RDONLY` set inside
`cdfs_mount` cannot survive on its own, so a caller who omitted the flag would
leave the kernel believing a CD is writable. The userland helper always passes
it; refusing anyone who does not is cheaper than depending on the helper being
the only caller.

## Two vectors with no compile-time checking

`struct vfsops` (8 slots) and `struct vnodeops` (37 slots) are K&R
`int (*)()` arrays. Nothing checks a signature, and nothing checks slot order:
the `VFS_*` and `VOP_*` call macros in `<sys/vfs.h>` and `<sys/vnode.h>` **are**
the ABI. Both vectors here are therefore written positionally, in the exact
struct order, with the slot number in a comment on every line. A slot inserted
in the wrong place compiles cleanly and calls the wrong function.

No `vnodeops` slot is left NULL — the generic vnode layer dereferences each one
unconditionally. Ops this read-only filesystem does not implement point at the
kernel's shared `fs_subr` stubs (`fs_nosys`, `fs_setfl`, `fs_frlock`, `fs_poll`,
`fs_pathconf`, `fs_cmp`, `fs_rwlock`, `fs_rwunlock`); every mutating op funnels
to `cdfs_rdonly()`, which returns `EROFS`.

## `<string.h>` is intercepted, and the reason is not source-level

The portable code calls `memcpy`/`memmove`/`memset`/`memcmp`/`strlen`, but the
SVR4 kernel resolves those from two places, not one: `ml/exp` exports **global
`memcpy` and `strlen`**, while **nothing** in the kernel provides `memmove` or
`memset`, and its only `memcmp` is a file-static in `netinet`. So `cdfs` must
define exactly `memmove`, `memset` and `memcmp` (in `kstring.c`) and must **not**
define `memcpy` or `strlen` — a second definition makes the whole-kernel
`ld -r` fail with "multiple definition".

[`compat/string.h`](compat/string.h) is placed ahead of the sysroot on the
include path for this build only, so `#include <string.h>` resolves there and
hands the portable code a minimal const-correct prototype set and nothing else.
It supplies **real symbols rather than a `#define memcpy odfs_kmemcpy`
redirect**, because a macro only catches source-level calls: GCC lowers
aggregate struct copies and zero-inits — such as `cnp->node = *n;`, where the
operand is a several-hundred-byte `odfs_node_t` — to direct `memcpy`/`memset` calls at
codegen time, which no macro can rewrite.

`memmove` is implemented by hand rather than delegated blindly to `bcopy`,
because **`bcopy` is not overlap-safe**: it tail-jumps into `mcpy`, which copies
strictly ascending, and the kernel's `memcpy` is merely a second entry point
into that same routine. `cdfs` has no `memmove` call site of its own, but
`memmove` is a global symbol in the whole-kernel link — so any other kernel code
binding to it was binding to a body that silently corrupts on destructive
overlap.

## No 32x32->64 multiply in the object — it must run on a 68060

The 68060 does not implement `MULU.L`/`MULS.L <ea>,Dh:Dl` (32x32->64) or
`DIVU.L`/`DIVS.L <ea>,Dr:Dq` (64/32); they trap to vector 61, and in kernel
mode nothing emulates them. gcc 2.7.2.3 has no `-m68060`, and with `-m68020`
or `-m68040` it emits the 64-bit multiply on its own for two idioms: a 32-bit
division or modulo by a **constant** (multiply by the reciprocal, keep the high
word) and `(uint64_t)a * b` with two 32-bit operands.

So the kernel arm defines `ODFS_NO_MUL64` (`include/odfs/config.h`), and the
portable code writes those idioms through [`odfs/arith.h`](../../include/odfs/arith.h):
`odfs_udiv32`/`odfs_umod32`/`odfs_sdiv32`/`odfs_smod32`/`odfs_udivsz` hide the
divisor from the optimizer so a plain 32-bit `DIVU.L`/`DIVS.L` is emitted, and
`odfs_umul64` builds the product from 16x16 partials. Without the knob every
helper is a macro for the open-coded operator, so the AmigaOS, host and AMIX
userspace builds compile to the same code as before. Power-of-two divisors need
none of this: they are shifts.

The check is a disassembly scan of the object for the trapping encodings, which
must find **zero** candidates:

```sh
m68k-cbm-sysv4-objdump -d build/amix-kernel/libcdfskern.o > cdfs.lst
scan060.py cdfs.lst            # test-tools/scan060.py in asokero/amix-040-060-port
```

A new division by a constant or a widening multiply in code that reaches the
kernel object brings a candidate back; route it through `odfs/arith.h`.

## The loadable module — `make -C module`

The same sources also build as a filesystem module for the Amix loadable-module
system (amix-dlm), one object per kernel line: `k30` (68030 kernels, 2 KiB
pages) and `k46` (68040/68060 kernels, 4 KiB pages). Both are module ABI
(kabi) 1.2.

```sh
make -C platform/amix-kernel/module         # build/amix-module/{k30,k46}/mod.d/cdfs
make -C platform/amix-kernel/module k30     # one line (or k46)
make -C platform/amix-kernel/module test    # the host test of the load path
```

It needs amix-dlm's built ABI kits of kabi `k30.1.2` / `k46.1.2` or later
(`DLM=/path/to/amix-dlm`, default the sibling checkout's `build/kit/<line>`;
or `KIT_k30=` / `KIT_k46=`), the pinned cross tools (`m68k-cbm-sysv4-*`) on
`PATH`, and `python3`.

**One source, two forms.** `-DCDFS_MODULE` selects the module form; without
it every file compiles to the static object, byte for byte (`libcdfskern.o`
md5 `694a6e147e6ac418a9f604a16b0636cb` before and after the switch). The
module form differs in three places, because a module may import only the
module DDI (`ddi.list`):

- `cdfsinit(row, index)` is run once by `vfs_register()`, which has already
  filled the row; it takes the index as the fstype and latches the
  `getudev()` major as the static one does. It does not search `vfssw[]`, so
  the module imports neither `vfssw` nor `nfstype` (`cdfs_modlocal.c`).
- The duplicate-drive guard keeps its own table of mounted drives instead of
  calling `vfs_devsearch()`: only a `cdfs` vfs carries the `cdfs` pseudo-major,
  so the question is the same. The drive is claimed in the guard and given back
  on every failed mount and at unmount.
- `strncpy` is the module's own (`kstring.c`).

The module's other objects are the static object's, compiled with the same
flags (`make amix-kernel-module AMIX_KIT=<kit>`) by the kit's pinned compiler
against the kit's headers, and linked in by the kit as `EXTRA`. The kit compiles
only the K&R wrapper `cdfs_mod.c` (it compiles with `-traditional`, which the
portable ANSI sources cannot take). The two lines' `libcdfskern.o` are
byte-identical: nothing in them depends on the page size. The kit's `sd.h`
has `SDCARDS` 4, so the module accepts SCSI cards 0–3; the static object,
built against the toolchain sysroot's `sd.h` (`SDCARDS` 2), accepts 0–1.

| Item | Content |
|---|---|
| `.moddata` | `name=cdfs class=boot match=always fs=cdfs unload=never desc=…`; `kabi=k30.1.2` / `k46.1.2`, `cpu=68030` / `68040`, `cc=` the kit's pin; no `majors`, no `param` (from [`module/keys`](module/keys)) |
| Wrapper | `MOD_MISC_WRAPPER(cdfs, cdfs_load, 0, …)` (`$modtype misc`): no switch-table row |
| `_load` | `vfs_register("cdfs", cdfsinit, &cdfs_vfsops, 0)` and nothing else: no I/O, no sleep, no allocation. `EEXIST` when the kernel already has a `cdfs` row (a static `cdfs`), `ENOSPC` when the spare rows are taken |
| `_unload`, `_halt` | none: a filesystem module is pinned, and `cdfs` holds no device state between requests |
| Imports | the module DDI only: `vfs_register`, `mod_miscops`, `sdqueue`, `sdopen`, `vn_rele`, `copyinstr`, `getudev`, `fs_nosys`, `fs_setfl`, `fs_frlock`, `fs_poll`, `fs_pathconf`, `fs_cmp`, `fs_rwlock`, `fs_rwunlock`, and the minor-0 names (`kmem_*`, `memcpy`, `strlen`, `strcmp`, `bcopy`, `cmn_err`, `sleep`, `wakeup`, `vtop`, `suser`, `uiomove`, `__udivdi3`, `__umoddi3`, `__lshrdi3`); plus the wrapper's weak `cdfs_conf_data` |
| Image | 69 704 bytes allocated |
| Version | [`module/VERSION`](module/VERSION), `1.0.0-1`, both lines |

**Class.** On a disk-rooted box the module is **boot class**: it loads at the
first `sysinit` entry, before any `rc` script or `/etc/vfstab` mount, because
the stock mount path has no hook that would load a filesystem by type on
demand. A box without a CD drive can turn it off with `disable` in
`modreg.local.d`. On a CD install medium `cdfs` is needed in the root pass, to
mount the disc as the package source; that root-class placement is to come
from the boot generation's composer promoting this same object, not from a
second object. So the object is built to run in place in the boot arena: its
sections and COMMONs are aligned to at most 16 and each lies at a file offset
that is a multiple of its alignment, and `_load` is safe at level 7 with the
clock stopped (it neither sleeps nor allocates; `cdfsinit` calls only
`getudev()`).

**Gates.** The kit's `modfix` and `modcheck.py`, then
[`module/modgate.py`](module/modgate.py): `nm -u` of the module lists only
DDI names (and the weak `cdfs_conf_data`), `fs=` equals the name `_load`
registers, the object passes the kit's run-in-place rule, and it prints the
`.moddata` keys and the sha256. [`module/hosttest.sh`](module/hosttest.sh)
runs `_load` → `vfs_register` → `cdfsinit(row, index)` on the host against a
stub `vfs_register` and `getudev`, and the drive table.

**What it needs to run.** A module kernel of kabi `k30.1.2` / `k46.1.2` whose
symbol block carries `vfs_register` and the thirteen names above that entered
at minor 2 (`sdqueue`, `sdopen`, `vn_rele`, `copyinstr`, `getudev` and the
eight `fs_*` helpers). The kabi 1.2 ABI text does not yet fingerprint the
layouts `cdfs` uses (`struct vfs`, `vnode`, `vfsops`, `vnodeops`, `vattr`,
`uio`, `dirent`, `statvfs`, `mounta`, `fid`, `sdcom`), so running the module
waits on the next kernel cut of each line, which carries those texts in the
ABI text and those names in the symbol block. Both lines run the stock 2.1 layouts today, which is why one
static object has run on both.

**Not yet done.**

- The module has not been loaded on any kernel: no kernel cut carries the
  names yet. Emulation, then hardware, follow (the mount and refusal rows of
  [`docs/amix-module-port.md`](../../docs/amix-module-port.md) §6).
- The `k46` object does not pass the kit's `modcheck.py` yet: its page-geometry
  rule refuses any shift by 11, and seven of the module's are CD-sector
  arithmetic (`offset / 2048` in the ISO 9660, Joliet and UDF readers and in
  `cdfs_bytes_to_dev_blocks`), not page arithmetic. The rule needs a way to
  tell them apart; every other `modcheck.py` and `modgate.py` check passes on
  `k46`.
- Root-class use waits on the promotion described above.
- The host test covers the load path and the drive table, not a mount.

## Known limits

- **Read-only**, by construction. `vfs_mountroot` returns `EROFS` and
  `vfs_swapvp` returns `EINVAL`: `cdfs` can be neither the root filesystem nor
  a swap device.
- **One 2048-byte sector per SCSI transfer.** Multi-sector requests came back as
  garbage on the bench HBA — the block cache's 8-sector read-ahead returned
  kernel text and then wedged the box — which is the signature of a DMA
  bounce/contiguity limit on that board. `read_sectors()` splits every request
  into single-block commands. Raise `AMIX_KERN_MAX_XFER` only once the HBA's DMA
  limit is known good.
- **ISO 9660 with Rock Ridge is the metal-proven path.** Every backend in
  `CORE_SRCS` compiles into the module, but High Sierra, Joliet, UDF, HFS, HFS+
  and CDDA have not been exercised end to end on hardware — see
  [`docs/amix-known-unknowns.md`](../../docs/amix-known-unknowns.md).
- **The node hash has no bucket locking.** v1 is single-threaded with respect to
  it; SMP hardening is a later step.
- **`vfs_vget` resolves only currently-cached nodes.** Stateless handle
  synthesis for NFS export is out of scope.
- **Not built in CI** — no cross toolchain is available there. See
  [`docs/testing.md`](../../docs/testing.md). On the host, `make check` covers
  `cdfs_blocks.h`, which was factored out of `cdfs_vnodeops.c` precisely so its
  arithmetic could be tested off-metal (`tests/unit/test_cdfs_blocks.c`), and a
  model of the read path (`tests/unit/test_read_span.c`).
