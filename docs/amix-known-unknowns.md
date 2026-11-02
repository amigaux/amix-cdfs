# AMIX port — known unknowns

Open questions about the AMIX (SVR4/m68k) target where we have solid *observations*
but no established *mechanism*. Kept separate from the code comments on purpose: an
unproven cause that hardens into a load-bearing comment is how a plausible guess
becomes a false fact.

Each entry states what is evidence, what is hypothesis, and what would settle it —
and, once settled, how. A resolved entry is kept here (marked RESOLVED, with its
evidence chain intact) rather than deleted: the record of *how* a mechanism was
finally pinned down is worth as much as the answer.

**Other repositories referenced below.** The AMIX port depends on work outside
this tree: the **[gcc-cross-amix](https://github.com/jusii/gcc-cross-amix)**
`m68k-cbm-sysv4` cross toolchain (compiler, binutils, `libgcc`), and an external
kernel-integration and on-metal validation harness (kernel assembly, the
whole-kernel link, the metal test cycle) that is not published. Commits cited
from the toolchain are qualified with its name wherever they appear.

---

## 1. `__udivdi3` returned a wrong value in the kernel (2026-07-19, RESOLVED 2026-07-21)

**Resolved on metal 2026-07-21.** The root cause was an assembler encoding bug in the
cross toolchain's `libgcc.a` — not anything in cdfs, and not the kernel link. The
evidence that opened this entry is kept below for the record; the resolution follows
it.

### What was established (the original observations)

On metal (A4000 + Z3660, kernel `sum -r /stand/unix` = 10842, cdfs at 31e8c3b), the
64-bit divide in `cdfs_getattr()`

```c
vap->va_nblocks = (u_long)((n->size + 2047u) / 2048u);
```

did not produce a quotient. m68k GCC 2.7.2.3 does not strength-reduce a DImode
division by a power of two, so this lowers to an out-of-line `__udivdi3` call, and
the value that came back fits

```
returned == (size + 2047) << 4
```

**exactly**, across four independent data points from one `ls -lR /cdrom`:

| observed | source | fit |
|---|---|---|
| 32848 | `SHORT.TXT`, 6 bytes | `(6 + 2047) * 16` |
| 33152 | `café.txt`, 25 bytes | `(25 + 2047) * 16` |
| 32912 | `total` of `deep/subdir` | one 10-byte entry |
| 65520 | `total` of `deep` | one 2048-byte directory extent |

`(size + 2047) << 4` is `(n0 << bm) >> 16` for libgcc `__udivdi3`'s own
normalization shift `bm = 32 - bitcount(2048) = 20` — i.e. the routine's **partial
remainder** after the first division digit, which for a 6-byte file is exactly
`0x80500000 >> 16 == 0x8050 == 32848`. The returned value is an internal
intermediate of the division loop.

The root directory's `total` printed as `4293100864`, i.e. a **negative** signed
`st_blocks` (`<sys/stat.h>` declares `long st_blocks`) that `ls` renders unsigned.

### Root cause (found 2026-07-21)

GNU **as 2.8.1** assembled gcc 2.7.2.3's SGS `tdivs.l` / `tdivu.l` — the
**32-bit-dividend** divide-with-remainder form — as the **64-bit-dividend** form,
setting the extension-word SIZE bit (`0x0400`) that selects a full 64-bit dividend
in `Dr:Dq`. The mis-encoded instruction then reads a 64-bit dividend whose high half
is a live loop intermediate, overflows, and — per the 68030's
**divide-overflow-leaves-the-registers-unchanged** behaviour — returns that
normalization intermediate untouched. That is exactly the `(size + 2047) << 4` value
tabulated above.

The bug was uniform, not per-call-site. Every DImode libgcc helper was affected —
`__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3`, and `__udivmoddi4` each carried
**8 broken divides**. Every divide in them was compiler-generated: the mc68020
inline-asm `udiv_qrnnd` path was never compiled in, because the libgcc build carried
no `-m68020`. So the fault lived in the helpers themselves, identically, wherever
they were linked.

**Fix:** commit `984192a` in the
**[gcc-cross-amix](https://github.com/jusii/gcc-cross-amix)** toolchain
repository (not this one) — an assembler wrapper that rewrites the mis-encoded forms to
`divsl.l` / `divul.l`; `libgcc.a` rebuilt 2026-07-21.

### Before / after on the same machine

Same physical A4000 + Z3660, same disc, single variable = the kernel's `libgcc` plus
its cdfs `exp`:

| `ls -ls /cdrom` | OLD kernel `sum -r` 10842 | NEW kernel `sum -r` 00961 |
|---|---|---|
| `total` (root) | 4293100864 | 24 |
| `SHORT.TXT` (6 B) | 32848 | 4 |
| `café.txt` (25 B) | 33152 | 4 |
| symlink | −2097152 | 0 |
| `deep` | 65520 | 4 |

The NEW kernel was booted with a fresh `libgcc` **and** a fresh cdfs `exp`, boot
chain rewritten, `bootchain-verify` PASS. Every value is correct.

### How it was isolated to `__udivdi3` (the nuance)

The NEW `exp` also carries the shift-based `st_blocks` fix — cdfs no longer divides
in `cdfs_getattr()` — so `ls -ls` being correct does **not** by itself prove the
helper. The isolating probe was a **userland exerciser statically carrying the
rebuilt `libgcc`'s `__udivdi3`**, run **on metal**:

| input | correct | rebuilt libgcc | old libgcc |
|---|---|---|---|
| `2053 / 2048` | q 1, r 5 | q 1, r 5 | (intermediate) |
| `(6 + 2047) / 2048` | 1 | 1 | 32848 |
| `(25 + 2047) / 2048` | 1 | 1 | 33152 |
| `1475108 / 2048` | q 720, r 548 | q 720, r 548 | (intermediate) |

All PASS with the rebuilt helper; the old helper returned the documented
intermediates on this same machine.

This **refutes the last standing binding-shaped hypothesis** — that a relocation
applied wrongly at kernel-link time corrupted the helper (item 3 of the old "what
would settle it"). Nothing about the kernel link changed the helper's correctness: a
userland binary that never touches the kernel link showed the identical fault and
the identical fix. The assembler encoding did it.

### Full gate

The on-metal gate (an external harness, not part of this repository), run
against the kernel with `sum -r` 00961 for 5 cycles = **PASS**: mount,
byte-identical CD reads vs `test_rr.iso` host truth, DMA canary clean 5/5.

### Hypotheses considered, and how each resolved

- **A missing / mis-resolved link-time binding.** Refuted 2026-07-19 by
  `nm /stand/unix` — `__udivdi3` was linked, global, a real `FUNC` at a sane text
  address with a plausible size, and its `__clz_tab` normalization table was present
  (two legal `LOCL` copies, values section-relative as `tools/strstat.py`
  documents):

  ```
  [6502] |  951104 | 1052 | FUNC | GLOB | 1 | __udivdi3
  [7295] |  952156 | 1128 | FUNC | GLOB | 1 | __umoddi3
  [4809] |  953284 |   84 | FUNC | GLOB | 1 | __lshrdi3
  [4710] |    3723 |  256 | OBJT | LOCL | 2 | __clz_tab
  [4713] |    3979 |  256 | OBJT | LOCL | 2 | __clz_tab
  ```

  Refuted again, harder, by the userland probe above.
- **A relocation applied wrongly at kernel-link time.** The one binding-shaped
  hypothesis `nm` could not exclude. Refuted by the userland probe: a binary with no
  kernel link showed the identical fault.
- **Z3660 CPU emulation of `divu.l`.** Ruled out 2026-07-19 on inspection of the UAE
  core's `m68k_divl()` — it decodes `Dq/Dr` and the `0x400` 64-bit-dividend bit
  correctly. That is fully *consistent* with the final cause: the emulator faithfully
  executed the mis-encoded 64-bit form; the wrong `0x400` bit was baked into the
  instruction stream by the assembler, not misdecoded at run time.
- **`/usr/sys/fs/libfsgcc.a` missing on the box.** A build-time staging artifact,
  never evidence — confirmed irrelevant.
- **"No native userland control is possible."** This was the wrong constraint. A
  **cross-compiled** exerciser built with the `gcc-cross-amix` toolchain (metal
  has only AT&T `cc` and no `libgcc.a`) *was* the control — and it is what
  settled the entry.

### Scope of the exposure (updated)

The ISO9660 / Rock Ridge read path was always empirically correct on metal (5/5,
byte-identical). It uses `__lshrdi3`, a different helper that the `divsl`/`divul` fix
does not touch and that was never mis-encoded.

`cdfs_getattr` was the only `__udivdi3` call site an ISO9660 mount exercised
per-file, and it no longer performs 64-bit division (shift-based `st_blocks`). The
remaining call sites still linked into `build/amix-kernel/libcdfskern.o` —

| function | helpers | when it runs |
|---|---|---|
| `odfs_iso_merge_multi_extent` | `__udivdi3`, `__umoddi3` | multi-extent ISO files only |
| `udf_get_volume_size` | `__udivdi3` | UDF mount |
| `hfs_probe`, `hfs_mount`, `hfs_read_node`, `hfs_get_volume_size` | `__udivdi3`, `__umoddi3` | HFS |
| `hfsp_mount`, `hfsp_read_fork`, `hfsp_get_volume_size` | `__udivdi3`, `__umoddi3` | HFS+ |

— remain **unexercised end-to-end on metal** (`cdda_get_volume_size` is no
longer among them: since module 1.1.0-5 the kernel build leaves CDDA out), so HFS / HFS+ / UDF probing is still
*unproven* as whole paths. But the specific failure mode that opened this entry is
**closed**: the underlying `__udivdi3` / `__umoddi3` helpers are now **metal-proven
correct** on this machine (see the userland exerciser above), so "unproven" no longer
means "the divide helper will hand back garbage." It means only that the paths
themselves have not yet been run.

To re-enumerate the helper call sites after any change:

```sh
make amix-kernel
m68k-cbm-sysv4-objdump -d --reloc build/amix-kernel/libcdfskern.o \
  | awk '/^[0-9a-f]+ </{fn=$2} /__udivdi3|__umoddi3|__lshrdi3/{print fn, $NF}'
```

### Standing rule

**Kept as defensive style guidance** — no longer a correctness workaround, but still
worth following: in AMIX kernel code, prefer a shift to a 64-bit division where the
divisor is a power of two. The helper is proven, so this is now about speed and
clarity — an out-of-line DImode divide on a 68030 is costly, and a toolchain-encoding
bug in exactly this instruction family is the kind that can resurface on a toolchain
rebuild. Cheap insurance, so it stays.
