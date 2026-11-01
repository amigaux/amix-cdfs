# AMIX (SVR4/m68k) — userspace side of cdfs

`cdfs` is the AMIX port of ODFileSystem: a read-only optical filesystem
(ISO9660 / High Sierra / Rock Ridge / Joliet / UDF) that lives *in the kernel*
(`platform/amix-kernel/`). This directory holds the userspace half — the media
backends used by the cross-built tools, and the **SVR4 mount helper** that makes
`mount -F cdfs` work at all.

## What this sits on

The kernel half is [`platform/amix-kernel/`](../amix-kernel/README.md) — the
SVR4 vfsops/vnodeops vectors, the vnode cache, and the in-kernel SCSI media
backend that DMAs sectors off the drive. Its README documents the four contracts
that half has to keep (no `VFS_INIT`; a self-locating `cdfsinit`; page-aligned,
non-page-crossing DMA targets; and never a `struct sdcom` on the kernel stack),
the anti-mount-stacking guard this document's hazard note refers to, and the
same errno table read from the kernel side.

## The mount helper — `/usr/lib/fs/cdfs/mount`

**Source:** [`mount.c`](mount.c) · **Build:** `make amix-mount` · **Artifact:** `build/amix/mount`

### Why it exists

SVR4's generic `mount(1M)` does not know how to mount anything itself. It parses
its arguments and then **execs `/usr/lib/fs/<fstype>/mount`**, handing that binary
the special and the mount point as the **last two positional arguments** (any
`-o` options precede them):

```
mount -F cdfs 0,2 /cdrom   →   exec /usr/lib/fs/cdfs/mount [-o ...] 0,2 /cdrom
```

So without this binary installed, `mount -F cdfs` **fails outright** — the kernel
filesystem is unreachable no matter how correct it is. It is a small file, but
the whole CD stack rests on it.

The helper does exactly two things:

1. **`mount(2)`**, the 6-argument `MS_DATA` form, fstype `"cdfs"`, always
   read-only:
   `mount(spec, dir, MS_DATA | MS_RDONLY, "cdfs", (char *)0, 0)`.
2. **Appends the `/etc/mnttab` entry** (see below).

### The mount spec is *not* a `/dev` path

cdfs addresses the drive directly, as `"<card><sep><unit>"` — `<sep>` is any run
of non-digit characters, so `0,2` / `0.2` / `0:2` are equivalent. `card` is the
SCSI host-adapter index, `unit` the target id; `cdfs_mount()` parses it kernel-side
(`platform/amix-kernel/cdfs_vfsops.c`, max 64 bytes).

There is no `/dev` node because a real CD never traverses `/dev/dsk` on stock
AMIX: `dd.c` hard-codes 512-byte blocks + an RDB, and stock `gsio`'s `iobuf[1024]`
is smaller than a 2048-byte CD block. Addressing by `(card,unit)` sidesteps that
entirely. Give the mount point as an **absolute** path — it is recorded as passed,
and (see below) it is also what `umount(1M)` will act on.

### It writes `/etc/mnttab` — because nothing else will

**The SVR4 kernel does not maintain `/etc/mnttab`; the mount commands do.** This
is the trap: skip the bookkeeping and the CD mounts and reads perfectly, yet is
invisible to `mount` (no args) and `df` (no args), and `umount` refuses it with
*"not in mnttab"*. The filesystem looks broken when it is not.

So on success the helper appends the standard TAB-separated, newline-terminated
5-field line:

```
special \t mountp \t fstype \t options \t time
/cdrom	/cdrom	cdfs	ro	1783866823
```

(alongside the box's own `/dev/dsk/c6d0s1  /  ufs  rw,suid  252647335`). fstype is
`cdfs`, options are always `ro`, and `time` is `time(0)` in decimal seconds.

If the mnttab append fails, the filesystem **is still mounted**: the helper warns
on stderr and **exits 0**. Never unmount a working filesystem because bookkeeping
failed.

### The `special` field is the **mount point** — not the spec

Look again at that line: the special is `/cdrom`, not the `0,2` the CD was mounted
from. That is deliberate, and it is the single subtlest thing in this file.

**`umount(1M)` unmounts by the mnttab `special` field** — not by the mount point
you hand it. It looks its argument up in `/etc/mnttab`, matches an entry, and then
unmounts whatever that entry's *first* field says. So writing cdfs's own spec there
asks the kernel to unmount `0,2`, which is not a path and cannot be unmounted — and
`umount(1M)` **still returns 0 and still deletes the entry anyway**.

Measured on the A4000+Z3660, with `0,2	/cdrom	cdfs	ro	<time>` in mnttab and the CD
accessed (`ls`, `mount`, `df`):

| mnttab `special` | `umount /cdrom` | mnttab entry | filesystem |
|---|---|---|---|
| `0,2` (the spec) | rc 0 | **removed** | **STILL MOUNTED** — `ls -la /cdrom` still lists the CD root |
| `/cdrom` (the mount point) | rc 0 | removed | **really unmounted** |

The spec form is *worse than no entry at all*: a missing entry merely produced a
"not in mnttab" warning, whereas a wrong one makes the bookkeeping **lie about
reality** — `mount` shows nothing mounted while the CD is still very much mounted.

Using the mount point as the special is the **established SVR4 convention for
filesystems with no block device**, and the box's own `/etc/mnttab` sets the
precedent:

```
/proc	/proc	proc	rw	...
/dev/fd	/dev/fd	fd	rw	...
```

cdfs is in exactly that category — its spec is a `<card>,<unit>` pair, not a `/dev`
path.

**The cost, stated honestly:** `mount` and `df` now show `/cdrom on /cdrom`, so the
mnttab entry **no longer records which drive is mounted** — the `0,2` is gone from
the bookkeeping (`mount(2)` is still given it, and the kernel still knows it). That
is an accepted trade for an `umount` that actually unmounts. It goes away only if
cdfs ever grows a real block-device spec; the hook for that, `devvp` in
`struct cdfs_vfs` ([`platform/amix-kernel/cdfs.h`](../amix-kernel/cdfs.h)), is
already there but unused.

### There is no cdfs `umount` helper — and none is possible

Unlike `mount(1M)`, **`umount(1M)` does not exec a per-fstype helper.** Probed
directly: a script planted at `/usr/lib/fs/cdfs/umount` was never executed, and
stock `/usr/lib/fs/ufs/` ships no `umount` either. So a umount helper is *not* an
alternative fix for the above — getting the `special` field right is the only
lever. Once the entry is correct, the generic `umount(1M)` unmounts the CD natively
and removes the entry itself.

**Hazard, seen on the bench box:** a *stray file* at `/usr/lib/fs/cdfs/umount`
(the probe script above, left behind) can later surface as

```
umount: cannot execute /usr/lib/fs/cdfs/umount
```

with exit 1 and **no `umount(2)` call at all** — the CD stays mounted, and every
further `mount -F cdfs` used to *stack* another mount on top of the stuck one
(rc=0; the kernel's own mount routines refuse that with `EBUSY` since the
anti-stacking guard in `cdfs_mount`, but the stray file still breaks the shell
path). **`/usr/lib/fs/cdfs/` must contain exactly one file: `mount`.** Remove
anything else:

```sh
rm -f /usr/lib/fs/cdfs/umount
```

### Exit status

| Status | Meaning |
|--------|---------|
| 0 | mounted (possibly with an mnttab warning on stderr) |
| 1 | `mount(2)` failed — message on stderr with the errno |
| 2 | usage error (fewer than 2 operands) |

This is also the exit status of `mount -F cdfs`. The stock `/sbin/mount`
(1991, 12 852 bytes) execs the helper in place: it imports `execv` and `exit`
but no `fork` or `wait`, and it only prints and exits 1 when the `execv` itself
fails. So a non-zero status from the helper is not lost by `mount(1M)`. A
script that runs `mount` in the background and collects it with `wait $pid`
can still see 0, because the Bourne shell may already have reaped a child that
failed at once. To check the status on a box, mount twice and read the second
status, which should be 1 (`errno 16`):

```sh
mount -F cdfs 0,5 /cdrom; mount -F cdfs 0,5 /cdrom; echo rc=$?; umount /cdrom
```

### Reading the errno of a failed mount

`mount(2)` returns an errno and nothing else, so the errno printed on status 1
is the **entire** diagnosis. That is why `e0ef11a` exists: every failure used to
arrive as errno 5, and an empty card slot, a drive that failed on the wire, a
disc with no filesystem on it and a 512-byte-block device were one
indistinguishable verdict — a merge that cost three separate investigations
before the cause of one of them was found. Each verdict now has its own arm in
`cdfs_errno()` ([`../amix-kernel/cdfs_node.c`](../amix-kernel/cdfs_node.c)):

| errno | What it means | First thing to check |
|---|---|---|
| `ENXIO` | No controller is registered at that **card** index — the address names nothing at all. `sdopen()` said so, and a `cmn_err` on the console names the card and `sdopen`'s return | The card number. Routine, not exotic: the stock kernel unregisters the a3091 once memory exceeds 64 MB, so `1,3` can be a slot with no controller in it |
| `ENOTTY` | The target answered INQUIRY but is **not** peripheral type 0x05 — the device is there, it is just not a CD drive. `ENODEV`/`ENXIO` would both read as "absent" and re-merge this with the case above; `ENOTTY` is produced **nowhere else in cdfs**, so it is an unambiguous fingerprint | The unit number: that target is a disk or a tape |
| `EINVAL` | The disc read fine and no backend recognized a filesystem on it, or its shape is one this driver does not serve (a block size other than 2048, a capacity past READ CAPACITY(10)) — or the spec string did not parse as `<card><sep><unit>` | The disc first, then the spec |
| `ENOMEM` | A kernel allocation failed | Kernel memory pressure |
| `EIO` | **Only** "the device did not deliver readable data" — nothing else routes here any more | Cable, termination, media |
| `EBUSY` | That drive is already mounted, or that directory already has a filesystem mounted on it. `cdfs` refuses to stack mounts; see the hazard note above | `mount` with no arguments |
| `EROFS` | `MS_RDONLY` was not passed. This helper always passes it, so seeing this means something else called `mount(2)` | The caller |
| `EPERM` | Not the superuser. `mount(2)` itself checks no privilege; every filesystem's `VFS_MOUNT` does | — |
| `ENOTDIR` | The mount point is not a directory | — |

### Building

Cross-build (the normal path — needs the
[gcc-cross-amix](https://github.com/jusii/gcc-cross-amix) toolchain):

```sh
make amix-mount          # just the helper  → build/amix/mount
make amix                # libodfs.a + imginfo/imgls/imgcat + mount
```

The result is an `ELF 32-bit MSB executable, Motorola m68k, 68020, (SYSV)`.

**On-box fallback** (the recovery route after an image rebuild — no cross
toolchain, no repo, just `mount.c` copied to the box and its own 1991 `cc`):

```sh
cc -O -o /usr/lib/fs/cdfs/mount mount.c
```

That fallback is *load-bearing*, and it is why `mount.c` is written in old-style
**K&R C** with no C99 and — deliberately — **no dependency on anything in this
repo**: no odfs headers, no `libodfs`, system headers only. The Makefile enforces
that mechanically by building it with neither `$(AMIX_INCLUDES)` nor
`$(AMIX_DEFS)`; the moment `mount.c` grows a repo dependency, that rule stops
compiling. Keep it standalone.

### Installing

```sh
mkdir -p /usr/lib/fs/cdfs
cp mount /usr/lib/fs/cdfs/mount
chmod 755 /usr/lib/fs/cdfs/mount
rm -f /usr/lib/fs/cdfs/umount   # a stray file here breaks umount(1M) — see above

mkdir -p /cdrom
mount -F cdfs 0,2 /cdrom        # SCSI card 0, target 2
mount                           # the CD now appears here — as "/cdrom on /cdrom"
df                              # … and here
umount /cdrom                   # … and this really unmounts it, entry and all
```

(`mount` reporting `/cdrom on /cdrom` rather than the `0,2` unit is expected — see
*The `special` field is the mount point* above.)

The `cdfs` row in the kernel's `vfssw[]` is added separately, when the kernel
is built; see [Getting the kernel filesystem into a kernel](../../README.md#getting-the-kernel-filesystem-into-a-kernel).
