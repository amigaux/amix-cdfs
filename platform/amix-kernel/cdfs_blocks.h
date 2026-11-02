/*
 * cdfs_blocks.h — vattr.va_nblocks (i.e. stat(2)'s st_blocks) for the AMIX
 * in-kernel cdfs.
 *
 * Factored into its own free-standing header for two reasons:
 *
 *   1. TESTABILITY. cdfs_vnodeops.c cannot be compiled on the host (it needs
 *      the SVR4 <sys/vnode.h> kernel headers), so the block-count arithmetic
 *      would otherwise be untestable anywhere but on metal. This header pulls
 *      in nothing at all, so tests/unit/test_cdfs_blocks.c can include it
 *      directly and `make check` covers the mapping.
 *
 *   2. NO 64-BIT DIVISION. See the hazard note below — this must stay 32-bit.
 *
 * ---- units ----
 *
 * SVR4 reports st_blocks in DEV_BSIZE units, NOT in filesystem-block units:
 *   <sys/param.h>:     #define DEV_BSIZE  512
 *                      #define NBPSCTR    512
 *                      #define SCTRSHFT   9
 *   <sys/sysmacros.h>: #define btod(BB)   (((BB) + NBPSCTR - 1) >> SCTRSHFT)
 * and <sys/stat.h> declares `long st_blocks` — a SIGNED long, so a bogus
 * large value shows up in `ls` totals as a huge unsigned number.
 *
 * An ISO9660/UDF file occupies whole 2048-byte logical sectors on the medium,
 * so the allocation is ceil(size / 2048) sectors, and each such sector is
 * 2048/512 == 4 DEV_BSIZE blocks. A 6-byte file therefore reports 4, which is
 * the same convention s5 uses on this box (1339-byte /etc/passwd -> 4).
 *
 * ---- hazard: never use 64-bit division here ----
 *
 * `n->size` is a uint64_t, but va_nblocks is a 32-bit u_long, so the natural
 * expression `(n->size + 2047) / 2048` is a 64-bit divide. On m68k GCC 2.7.2.3
 * that is NOT strength-reduced to a shift: it emits an out-of-line call to
 * __udivdi3.
 *
 * OBSERVED on metal (2026-07-19): that call returned an internal intermediate
 * of libgcc __udivdi3's normalization step instead of the quotient. The
 * returned value fit `(size + 2047) << 4` exactly across four independent data
 * points (two files, two directory totals) — which is `(n0 << bm) >> 16` for
 * the routine's own bm=20, i.e. its partial remainder.
 *
 * RESOLVED (2026-07-21): an assembler encoding bug in the cross toolchain's
 * libgcc. GNU as 2.8.1 assembled the 32-bit-dividend divide-with-remainder
 * form as the 64-bit-dividend form, so every divide in the DImode helpers
 * (__udivdi3, __umoddi3, __divdi3, __moddi3, __udivmoddi4) read a stale high
 * half and overflowed. Fixed in gcc-cross-amix 984192a and libgcc rebuilt;
 * the full record is §1 of docs/amix-known-unknowns.md.
 *
 * The shift stays on its own merits: va_nblocks is 32-bit, so this arithmetic
 * never needed 64 bits in the first place. A 32-bit `>>` is inline m68k code
 * and calls nothing.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef CDFS_BLOCKS_H
#define CDFS_BLOCKS_H

/* ISO9660 / UDF logical sector: 2048 == 1 << 11 */
#define CDFS_LOG_SECTOR_SIZE    2048UL
#define CDFS_LOG_SECTOR_SHIFT   11

/* SVR4 DEV_BSIZE: 512 == 1 << 9 (<sys/param.h>) */
#define CDFS_DEV_BSIZE          512UL
#define CDFS_DEV_BSHIFT         9

/*
 * Bytes -> DEV_BSIZE (512-byte) blocks, rounded up to a whole 2048-byte
 * logical sector.
 *
 * Takes an unsigned long (32-bit on AMIX), not a uint64_t, on purpose: the
 * caller truncates n->size the same way va_size does, because the SVR4 vattr
 * cannot express a size >= 4 GB in the first place. Keeping the truncation at
 * the call site keeps va_size and va_nblocks consistent with each other.
 *
 * No overflow: the largest input 0xFFFFFFFF yields 0x200000 sectors, and
 * 0x200000 << 2 == 0x800000 — comfortably inside a signed 32-bit st_blocks.
 */
static unsigned long cdfs_bytes_to_dev_blocks(unsigned long bytes)
{
    unsigned long sectors;

    /* Round up by reconstructing rather than by the usual `bytes + 2047`: the
     * latter wraps for sizes within 2 KB of 4 GB and would report a nearly-4 GB
     * file as occupying almost nothing. (GCC 2.7.2.3 compiles this to the same
     * lsr / bftst / addq either way, so the safety is free.) */
    sectors = bytes >> CDFS_LOG_SECTOR_SHIFT;
    if ((sectors << CDFS_LOG_SECTOR_SHIFT) != bytes)
        sectors++;

    return sectors << (CDFS_LOG_SECTOR_SHIFT - CDFS_DEV_BSHIFT);
}

#endif /* CDFS_BLOCKS_H */
