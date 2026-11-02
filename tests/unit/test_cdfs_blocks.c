/*
 * test_cdfs_blocks.c — host coverage for the AMIX in-kernel cdfs st_blocks
 * mapping (platform/amix-kernel/cdfs_blocks.h).
 *
 * cdfs_vnodeops.c itself cannot be built on the host (it needs the SVR4
 * <sys/vnode.h> kernel headers), which is exactly why the arithmetic lives in
 * a free-standing header: this test reaches it directly.
 *
 * Regression origin: on metal (A4000+Z3660, 2026-07-19) `ls -ls /cdrom`
 * reported 32848 blocks for a 6-byte file and a negative directory total. The
 * old expression was a 64-bit divide, `(n->size + 2047u) / 2048u`, which m68k
 * GCC 2.7.2.3 lowers to an out-of-line __udivdi3 call. On metal that call
 * returned an intermediate of libgcc's normalization step rather than the
 * quotient: 32848 == (6 + 2047) << 4 and 33152 == (25 + 2047) << 4. (The
 * cause, found 2026-07-21, was an assembler encoding bug in the toolchain's
 * libgcc; see docs/amix-known-unknowns.md.) Both values are pinned below as
 * must-never-return fingerprints.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "../../platform/amix-kernel/cdfs_blocks.h"
#include "test_harness.h"

/* One 2048-byte logical sector == 4 DEV_BSIZE (512 B) blocks. */
#define SECT 4UL

TEST(constants_match_svr4)
{
    /* SVR4 <sys/param.h>: DEV_BSIZE 512, NBPSCTR 512, SCTRSHFT 9. */
    ASSERT_EQ(CDFS_DEV_BSIZE, 512UL);
    ASSERT_EQ(1UL << CDFS_DEV_BSHIFT, CDFS_DEV_BSIZE);

    /* ISO9660 / UDF logical sector. */
    ASSERT_EQ(CDFS_LOG_SECTOR_SIZE, 2048UL);
    ASSERT_EQ(1UL << CDFS_LOG_SECTOR_SHIFT, CDFS_LOG_SECTOR_SIZE);
}

TEST(empty_file_occupies_nothing)
{
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(0UL), 0UL);
}

TEST(short_files_occupy_one_whole_sector)
{
    /* Anything from 1 byte to a full sector rounds up to one 2048-byte
     * sector == 4 DEV_BSIZE blocks. This matches the s5 convention on the
     * same box, where a 1339-byte /etc/passwd reports 4. */
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(1UL),    SECT);
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(6UL),    SECT);   /* SHORT.TXT */
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(25UL),   SECT);   /* café.txt  */
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(2047UL), SECT);
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(2048UL), SECT);
}

TEST(sector_boundaries_round_up)
{
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(2049UL), 2UL * SECT);
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(4095UL), 2UL * SECT);
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(4096UL), 2UL * SECT);
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(4097UL), 3UL * SECT);
}

TEST(large_sizes)
{
    /* 1 MiB == 512 sectors == 2048 DEV_BSIZE blocks. */
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(1048576UL), 512UL * SECT);

    /* Largest representable input must stay positive in a signed 32-bit
     * st_blocks — a negative st_blocks is what produced the garbage `total`
     * line that opened this investigation. */
    ASSERT_EQ(cdfs_bytes_to_dev_blocks(0xFFFFFFFFUL), 0x800000UL);
    ASSERT(cdfs_bytes_to_dev_blocks(0xFFFFFFFFUL) <= 0x7FFFFFFFUL);
}

TEST(never_returns_the_metal_garbage)
{
    /* The exact values observed on metal before the fix. If a future change
     * reintroduces a 64-bit divide (or otherwise resurrects the __udivdi3
     * path), these are the fingerprints to catch it by. */
    ASSERT_NE(cdfs_bytes_to_dev_blocks(6UL),  32848UL);
    ASSERT_NE(cdfs_bytes_to_dev_blocks(25UL), 33152UL);

    /* ...and the shape they came from: numerator << 4. */
    ASSERT_NE(cdfs_bytes_to_dev_blocks(6UL),  (6UL  + 2047UL) << 4);
    ASSERT_NE(cdfs_bytes_to_dev_blocks(25UL), (25UL + 2047UL) << 4);
}

TEST(monotonic_and_sector_quantised)
{
    unsigned long prev = 0UL;
    unsigned long bytes;

    /* Sweep several sectors' worth: never decreasing, always a whole number
     * of sectors, always enough to hold the file, and never wasteful by more
     * than one sector. */
    for (bytes = 0UL; bytes <= 16384UL; bytes++) {
        unsigned long blocks = cdfs_bytes_to_dev_blocks(bytes);

        ASSERT(blocks >= prev);
        ASSERT_EQ(blocks % SECT, 0UL);
        ASSERT(blocks * CDFS_DEV_BSIZE >= bytes);
        ASSERT(blocks * CDFS_DEV_BSIZE < bytes + CDFS_LOG_SECTOR_SIZE);

        prev = blocks;
    }
}

TEST_MAIN()
