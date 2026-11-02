# amix-cdfs — ODFileSystem with an AMIX (SVR4/m68k) port

> **This is an independent fork of [reinauer/ODFileSystem](https://github.com/reinauer/ODFileSystem)**
> by Stefan Reinauer and contributors, maintained by Jussi Alanärä. It adds **AMIX** —
> Commodore's SVR4 UNIX for the Amiga (68030, and 68040/68060 kernels) — as a target
> beside AmigaOS 3/4: a userspace build (`make amix`: `libodfs.a`, `imginfo`/`imgls`/`imgcat`,
> the SVR4 `mount` helper) and an in-kernel read-only filesystem (`make amix-kernel`,
> mounted with `mount -F cdfs`). Everything AMIX-specific lives in `platform/amix/`,
> `platform/amix-kernel/` and `#if ODFS_PLATFORM_AMIX*` branches; the AmigaOS handler and
> host builds are unchanged.
>
> It is **not** an upstream-merged tree: it forked at upstream `00ae148` (2026-07-06) and
> does not carry upstream's later commits (see [docs/upstream-sync.md](docs/upstream-sync.md)).
> For AmigaOS use, get ODFileSystem from upstream. Report AMIX issues here, not upstream.
> Trademark notes are in [NOTICE](NOTICE).

The original README follows, with the AMIX chapter added.

---

## ODFileSystem

ODFileSystem is a read-only optical-disc filesystem driver for Amiga systems. It is implemented as an AmigaDOS handler frontend with clean backend plugins for the various optical-disc formats it supports, allowing it to mount and browse CD-ROM, DVD, Blu-ray, and image-based media.

The project also includes host-side tools and tests so most parser and cache logic can be developed and validated outside an Amiga environment.

In this fork, the same core also builds for AMIX — Commodore's SVR4/m68k UNIX — where it runs as an in-kernel filesystem mounted with `mount -F cdfs`. See [Building for AMIX](#building-for-amix-svr4m68k).

## Purpose

ODFileSystem is the most modern and complete optical-disc filesystem for the Amiga. The handler deals with AmigaDOS packets, locks, and file handles, while the core library and backend parsers detect media formats, choose the appropriate view of hybrid discs, enumerate directories, and read file data.

ODFileSystem can:

- mount optical media through a standard AmigaDOS handler
- support common CD/DVD filesystem formats and hybrid discs
- present a consistent read-only file and directory model to Amiga software
- allow most logic to be tested on a host machine using image files

## Supported Filesystems And Features

### Filesystems

ODFileSystem currently includes backends for:

- ISO 9660
- High Sierra
- Rock Ridge
- Joliet
- UDF
- HFS
- HFS+
- CDDA virtual audio tracks

### Features

- Read-only AmigaDOS handler for optical media
- Multisession support, with the last session selected by default
- Hybrid-disc precedence rules
- Deterministic duplicate-name handling after normalization or charset conversion
- Rock Ridge, Joliet, UDF, HFS, and HFS+ probing
- Rock Ridge and UDF symlinks exposed as Amiga soft links
  (`ST_SOFTLINK` / `ACTION_READ_LINK`)
- Path lookup, directory enumeration, and file reads
- Block-cache based media access
- Host-side tools for inspecting image files
- AMIX (SVR4/m68k) in-kernel `cdfs` filesystem, plus cross-built AMIX tools

### Feature Matrix

| Filesystem | Status | Notes |
| --- | --- | --- |
| ISO 9660 | Supported | Plain ISO 9660 names and directory traversal |
| High Sierra | Supported | Pre-ISO 9660 (1986) variant, detected automatically; not in the ROM profile |
| Rock Ridge | Supported | Preferred over plain ISO when present |
| Joliet | Supported | Preferred over plain ISO when Rock Ridge is absent |
| UDF | Supported | Bridge discs default to ISO-family content unless forced |
| HFS | Supported with limitations | Data fork only |
| HFS+ | Supported with limitations | Data fork only; resource forks are not exposed |
| CDDA | Supported | Exposed as virtual WAV files |

### Format Selection Rules

For ISO-family hybrids, the default precedence is:

1. Rock Ridge
2. Joliet
3. Plain ISO 9660

For bridge and hybrid discs:

- ISO-family content is preferred over UDF by default
- ISO-family content is preferred over HFS by default
- UDF can be preferred explicitly
- HFS can be preferred explicitly
- A specific session or backend can be forced by mount options

UDF and ISO on the same disc are usually a bridge layout, where the ISO-family
side exists for older systems and the UDF side exists for newer ones. On Amiga,
the ISO-family view is the safer default because:

- Rock Ridge and Joliet map more naturally to the naming and metadata path the
  handler already exposes
- hybrid optical discs historically expected older systems to use the ISO side
- plain ISO-family trees are generally the least surprising fallback
- UDF support is newer and more likely to hit edge cases on odd media

So the default is essentially:

- prefer the more traditional, compatibility-oriented view
- let the user opt into UDF when they actually want that view

That is why the handler has an explicit `UDF` override instead of making UDF
the default on bridge discs.

To prefer UDF for a bridge disc from the Mountlist, add:

```text
Control = "UDF"
```

You can combine that with other control flags, for example:

```text
Control = "UDF LOWERCASE FILEBUFFERS=128"
```

### Name Collision Policy

If multiple on-disc names normalize to the same visible AmigaDOS name, the first
entry keeps the unsuffixed name and later entries are renamed deterministically
in on-disc order using `~2`, `~3`, and so on.

### HFS And HFS+ Limitations

- Only data forks are exposed. Resource forks and Finder metadata are not
  presented as separate files or alternate streams.
- Because of those limits, some classic Mac software distributions may be
  incomplete when viewed through ODFileSystem even though their main data files
  remain readable.

### CDDA

Audio tracks are exposed as virtual WAV files. On mixed-mode discs they appear in a `CDDA/` directory, and on pure audio discs they appear at the root.

### Amiga Rock Ridge AS

ODFileSystem now parses the Amiga-specific Rock Ridge `AS` SUSP entry on top of normal RRIP handling.

Current behavior:

- preserve the raw 4-byte Amiga protection field when present
- expose Amiga comments through the Amiga handler metadata path
- continue to use standard RRIP for names, timestamps, symlinks, and directory structure

Real-world `AS` source images used during development:

- [Arabian Nights (1993)(Buzz)(M4)](https://archive.org/download/noaen-tosec-iso-commodore-amiga-cd32/Commodore/Amiga%20CD32/Games/%5BBIN%5D/Arabian%20Nights%20%281993%29%28Buzz%29%28M4%29.7z)
- [Benefactor (1994)(Psygnosis)](https://archive.org/download/noaen-tosec-iso-commodore-amiga-cd32/Commodore/Amiga%20CD32/Games/%5BIMG%5D/Benefactor%20%281994%29%28Psygnosis%29.7z)

The automated real-image golden test downloads only the smaller `Arabian Nights` archive on demand, verifies the Archive.org MD5 before reuse, extracts track 1 to a plain 2048-byte data image, and skips cleanly if download or extraction tooling is unavailable. If a prepared data-track image already exists locally, set `ODFS_REAL_AS_IMAGE=/path/to/arabian_nights.iso` to reuse it without redownloading. The larger `Benefactor` image is kept as a manual reference input.

## Building for AmigaOS

The Amiga handler is built with the `amiga` make target. The Makefile selects
the Amiga frontend from the compiler target:

- `m68k-amigaos-gcc` builds the AmigaOS 3/classic handler
- `ppc-amigaos-gcc` builds the native AmigaOS 4 handler

### AmigaOS 3

With an m68k AmigaOS cross-compiler in `PATH`, run:

```sh
make amiga
```

This uses `m68k-amigaos-gcc` by default and writes:

```text
build/amiga/ODFileSystem
```

If the compiler is not in `PATH`, pass it explicitly:

```sh
make amiga CC=/path/to/m68k-amigaos-gcc
```

If the matching `ar`, `size`, and `strip` tools are also outside `PATH`, pass
those too:

```sh
make amiga \
  CC=/path/to/m68k-amigaos-gcc \
  AMIGA_AR=/path/to/m68k-amigaos-ar \
  AMIGA_SIZE=/path/to/m68k-amigaos-size \
  STRIP=/path/to/m68k-amigaos-strip
```

To build the OS3 release archive, use:

```sh
make amigaos3-lha
```

This creates `build/amiga/ODFileSystem.lha` containing `ODFileSystem`,
`ODFileSystem-test`, `ODFileSystem-rom`, `ODFileSystem-rom-test`, and
`README.md`.  The test ADF is built and released separately.

### AmigaOS 4

With the AmigaOS 4 PPC toolchain in `PATH`, run:

```sh
make amiga CC=ppc-amigaos-gcc
```

This selects `AMIGA_TARGET=os4` automatically and builds the native OS4
filesystem handler. To keep OS3 and OS4 outputs side by side, use a separate
build directory:

```sh
make amiga \
  CC=ppc-amigaos-gcc \
  AMIGA_BUILD=build/amiga-os4
```

The handler is then written to:

```text
build/amiga-os4/ODFileSystem
```

For OS4 builds the Makefile also writes a Kickstart-module form:

```text
build/amiga-os4/CDFileSystem
```

These two files are intentionally different:

- `ODFileSystem` is the normal disk-loadable filesystem handler. Copy it to
  `L:ODFileSystem` and use it with a DOSDriver or mountlist.
- `CDFileSystem` is a relocatable Kickstart resident module. Use it when
  replacing `Kickstart/CDFileSystem` in an OS4 Kickstart directory or
  `Kickstart.zip`; the Kicklayout entry remains `MODULE Kickstart/CDFileSystem`.

If the OS4 toolchain is installed outside `PATH`, pass the full tool paths:

```sh
make amiga \
  CC=/opt/amiga-ppc/bin/ppc-amigaos-gcc \
  AMIGA_AR=/opt/amiga-ppc/bin/ppc-amigaos-ar \
  AMIGA_OBJCOPY=/opt/amiga-ppc/bin/ppc-amigaos-objcopy \
  AMIGA_SIZE=/opt/amiga-ppc/bin/ppc-amigaos-size \
  STRIP=/opt/amiga-ppc/bin/ppc-amigaos-strip \
  AMIGA_BUILD=build/amiga-os4
```

The Makefile normally derives the NDK include path from the selected compiler.
If your SDK is elsewhere, add `NDK_PATH=/path/to/include_h`.

### Debug and Size Limits

Release builds have serial logging disabled. For a test build with serial
output enabled, use `make amiga-test` with the same toolchain selection:

```sh
make amiga-test
make amiga-test CC=ppc-amigaos-gcc AMIGA_TEST_BUILD=build/amiga-os4-test
```

The OS4 test build likewise produces both `ODFileSystem` and `CDFileSystem`
under the selected test build directory.

To build the OS4 release archive, use:

```sh
make amigaos4-lha \
  CC=ppc-amigaos-gcc \
  AMIGA_BUILD=build/amiga-os4 \
  AMIGA_TEST_BUILD=build/amiga-os4-test
```

This creates `build/amiga-os4/ODFileSystem-amigaos4.lha` containing
`ODFileSystem-amigaos4`, `CDFileSystem`, `ODFileSystem-amigaos4-test`,
`CDFileSystem-test`, and `README.md`.

Release builds enforce a default size limit of `60000` bytes for OS3 and
`131072` bytes for OS4. If intentional growth needs a higher ceiling, override
it with `AMIGA_SIZE_LIMIT=<bytes>`. During local bring-up, the limit can be
disabled with `ENFORCE_SIZE_LIMITS=0`.

## Sample Mountlist

For the mountlist examples below, copy the built handler to
`L:ODFileSystem`. With the default build directory that is
`build/amiga/ODFileSystem`; if you set `AMIGA_BUILD`, use the corresponding
`ODFileSystem` output from that directory.

For Workbench-style installation, copy:

- `platform/amiga/dosdrivers/CD0` to `DEVS:DOSDrivers/CD0`
- `platform/amiga/dosdrivers/CD0.info` to `DEVS:DOSDrivers/CD0.info`

Change `FileSystem` in CD0 to L:ODFileSystem.

Then edit the `Device` and `Unit` tooltypes on the `CD0` icon to match your
hardware.

Only one active DOSDriver may use a given device name. If another CD
filesystem is already mounted as `CD0:`, either replace or disable that
entry before mounting ODFileSystem as `CD0:`, or rename the ODFileSystem
DOSDriver to another name such as `OD0:`.

If you want a plain Mountlist entry instead, add one such as:

```text
CD0:
    FileSystem = L:ODFileSystem
    Stacksize = 16384
    Priority  = 5
    GlobVec   = -1
    DosType   = 0x43443031
    ForceLoad = 1
    Device    = scsi.device
    Unit      = 2
    Flags     = 0
    Surfaces  = 1
    BlocksPerTrack = 1
    BlockSize = 2048
    Reserved  = 0
    LowCyl    = 0
    HighCyl   = 0
    Buffers   = 20
    BufMemType = 0
    Mount     = 1
    Activate  = 1
#
```

`ForceLoad = 1` matters on systems whose controller ROM registers a CD
filesystem for DosType 'CD01' in FileSystem.resource (the A4091 does):
without it, Mount silently uses that ROM filesystem instead of
`L:ODFileSystem` and overrides the mountlist StackSize.

Optional handler control flags can be supplied through the mount entry control string, for example:

```text
Control = "LOWERCASE UDF FILEBUFFERS=128"
```

Supported control flags:

- `LOWERCASE` to lowercase plain ISO names
- `NOROCKRIDGE` or `NORR` to disable Rock Ridge
- `NOJOLIET` or `NOJ` to disable Joliet
- `HFSFIRST` or `HF` to prefer HFS on hybrid discs
- `UDF` to prefer UDF on bridge discs
- `FILEBUFFERS` or `FB` to set the block-cache size

See [mountlist.example](docs/mountlist.example)
for a fuller example with hardware notes, and
[CD0](platform/amiga/dosdrivers/CD0) for the
packaged DOSDriver entry used with Workbench.

## Building for AMIX (SVR4/m68k)

AMIX is Commodore's SVR4 UNIX for the 68030 Amiga. It is a full target beside
AmigaOS 3 and 4, running the same portable core and backend parsers behind two
new frontends: a set of cross-built tools that read images and raw devices, and
an **in-kernel SVR4 vfs/vnode filesystem** named `cdfs`, which `mount -F cdfs`
mounts like any other SVR4 filesystem. The AmigaDOS handler is not involved and
is not changed by any of it.

### The two build arms

```sh
make amix          # libodfs.a + imginfo/imgls/imgcat + the mount helper
make amix-kernel   # one relocatable kernel object
make -C platform/amix-kernel/module   # the same, as a loadable module (k30, k46)
```

`make amix` writes `build/amix/`: `libodfs.a` cross-built for AMIX, the three
`img*` tools linked against it, and `mount` — the SVR4 mount helper installed on
the box as `/usr/lib/fs/cdfs/mount`. The library pairs the shared core with two
AMIX media backends: `platform/amix/amix_file_media.c` reads an ISO image or a
raw disk slice through `open`/`lseek`/`read`, and `platform/amix/amix_scsi_media.c`
talks to a real drive through the `/dev/scsi` GSIO ioctl.

`make amix-kernel` writes `build/amix-kernel/libcdfskern.o`: the same core and
backends compiled for the kernel (`-DODFS_PLATFORM_AMIX_KERNEL -D_KERNEL`, no
libc, no syscalls), plus the vfsops/vnodeops/node-cache glue and an in-kernel
SCSI media backend, combined by `ld -r` into one relocatable object. This is the
actual deliverable; see [platform/amix-kernel/README.md](platform/amix-kernel/README.md).

The two arms are independent of each other and of `host`, `amiga` and `check`,
and write to their own build directories.

### Toolchain

Both arms cross-compile with `m68k-cbm-sysv4-gcc` from the
[gcc-cross-amix](https://github.com/jusii/gcc-cross-amix) toolchain — GCC
**2.7.2.3**, the period compiler. That is why the shared
`core/` and `backends/` code is kept C89-clean: no `for`-loop initial
declarations, no declarations after statements.

The Makefile looks for the toolchain under `/opt/amix-cross`, that build's
default install prefix. Point it
elsewhere with `AMIX_CROSS_PREFIX`, or override the individual tools:

```sh
make amix        AMIX_CROSS_PREFIX=/path/to/amix-cross
make amix-kernel AMIX_CROSS_PREFIX=/path/to/amix-cross
```

| Variable | Default | Purpose |
| --- | --- | --- |
| `AMIX_CROSS_PREFIX` | `/opt/amix-cross` | Root of the cross toolchain |
| `AMIX_CC` | `$(AMIX_CROSS_PREFIX)/bin/m68k-cbm-sysv4-gcc` | The cross compiler, for both arms |
| `AMIX_AR` | `ar` | Archiver for `build/amix/libodfs.a`. The host `ar` by default: the toolchain's GNU ar 2.8.1 aborts on archive creation, and the cross `ld` reads the host armap fine |
| `AMIX_LD` | `$(AMIX_CROSS_PREFIX)/bin/m68k-cbm-sysv4-ld` | Used for the `ld -r` that combines the kernel objects |
| `AMIX_SYSROOT` | `$(AMIX_CROSS_PREFIX)/m68k-cbm-sysv4/sysroot` | Where the two SCSI backends find the kernel's `rico.h`/`sd.h` |

### Why there is a fixed-width-type shim

The AMIX sysroot ships neither `<stdint.h>` nor `<inttypes.h>`, and GCC 2.7.2.3
predates both — so the shared code's existing `#include <stdint.h>` lines have
nothing to resolve against. [`include/odfs/compat_stdint.h`](include/odfs/compat_stdint.h)
supplies exactly the set this tree uses, and `platform/amix/compat/stdint.h` and
`platform/amix/compat/inttypes.h` forward to it. That compat directory is placed
ahead of the sysroot on the include path, so no shared source file had to be
edited. The whole body is gated on `ODFS_PLATFORM_AMIX` /
`ODFS_PLATFORM_AMIX_KERNEL`, so host and AmigaOS builds never see any of it.

### Getting the kernel filesystem into a kernel

`make amix-kernel` produces a relocatable object, not a loadable module: stock
AMIX has no loadable filesystems. (For kernels of the Amix loadable-module
system, the same sources build as a demand-class filesystem module, loaded
at the first `mount -F cdfs`, with
`make -C platform/amix-kernel/module`; see
[platform/amix-kernel/README.md](platform/amix-kernel/README.md#the-loadable-module--make--c-module).)
The object has to be built into a kernel, and
this repository's Makefile does not do that step. In the stock `/usr/sys`
kernel tree it takes three edits and a relink:

1. install `libcdfskern.o` as `fs/cdfs/exp` and add `cdfs/exp` to
   `OPT_SUBEXP` in `fs/Makefile`;
2. in `master.d/filesys.c`, declare `extern int cdfsinit();` and add the row
   `{ "cdfs", cdfsinit, 0, 0 }` to the static `vfssw[]` table;
3. put the cross toolchain's `libgcc.a` on the kernel's final `ld` line, so the
   64-bit helpers the object leaves undefined (`__udivdi3`, `__umoddi3`,
   `__lshrdi3`) resolve — the stock kernel carries no `libgcc`;

then relink the kernel, install it as `/stand/unix` and boot it. The general kernel rebuild procedure is
described in the AMIX grimoire,
[Building & Installing a Kernel](https://github.com/jusii/grimoire-amix/blob/master/docs/drivers/kernel-build.md).
The scripted kernel-integration harness this port was validated with is not
published yet.

### Mounting

With such a kernel booted and `build/amix/mount` installed as
`/usr/lib/fs/cdfs/mount` (mode 755):

```sh
mkdir -p /cdrom
mount -F cdfs 0,2 /cdrom     # SCSI card 0, target 2
```

The mount *special* is not a `/dev` path. It is `"<card><sep><unit>"`, where
`<sep>` is any run of non-digit characters — `0,2`, `0.2` and `0:2` are the same
thing — naming the SCSI host-adapter index and the target id. There is no `/dev`
node because a real CD never traverses `/dev/dsk` on stock AMIX: `dd.c`
hard-codes 512-byte blocks plus an RDB, and stock `gsio`'s `iobuf[1024]` is
smaller than a single 2048-byte CD block. Addressing by `(card,unit)` sidesteps
both.

[platform/amix/README.md](platform/amix/README.md) covers the mount helper in
full: why SVR4 cannot mount anything without it, what it writes to
`/etc/mnttab` and why, and how to read the errno of a failed mount.

### Known limits

- Read-only, as on every other target.
- **ISO 9660 with Rock Ridge** is the path proven end to end on real hardware
  (A4000 + Z3660). High Sierra, Joliet, UDF, HFS and HFS+ are compiled into
  both AMIX arms but have not been exercised end to end there — see
  [docs/amix-known-unknowns.md](docs/amix-known-unknowns.md). CDDA is in the
  userspace arm only; the kernel build leaves it out.
- The in-kernel SCSI backend caps every transfer at **one 2048-byte sector**:
  a transfer must stay inside one physical page, because kernel memory is
  physically scattered and a SCSI command carries one physical address. The
  backend splits each read into single-sector commands and has no block-cache
  read-ahead. Correct, but slower than the hardware allows.
- Neither AMIX arm is built in CI — no cross toolchain is available there. They
  are validated out of band; see [docs/testing.md](docs/testing.md).

## Unit Tests

Unit tests are host-side tests under `tests/unit/`. Run them with:

```sh
make check
```

This builds the host library and the unit-test binaries, then runs all suites.

The repository also contains image-based golden tests in `tests/golden/`, but the unit-test entry point is `make check`.

`make golden-check` also includes an optional real-world `AS` fixture test. It uses `tests/golden/fetch_real_as_fixture.sh` to cache and verify the small `Arabian Nights` archive locally before running the metadata assertions.

## License

ODFileSystem is licensed under the BSD 2-Clause license. See [LICENSE](LICENSE) for the full text.
