/*
 * test_kstring.c — host coverage for the AMIX in-kernel mem primitives
 * (platform/amix-kernel/kstring.c).
 *
 * kstring.c is compiled here as it is for the kernel, with stand-ins for the
 * two kernel routines it calls: bzero, which counts its calls so the test can
 * see that memset(.., 0, ..) goes to it, and a strictly forward bcopy, as the
 * kernel's is. memset, memmove and memcmp are checked byte for byte against
 * the C library over every head alignment, a range of lengths, and fill
 * values with the high bit set; the bytes either side of the target must not
 * change.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define _DEFAULT_SOURCE 1   /* caddr_t in <sys/types.h> */

#include "test_harness.h"

#include <stddef.h>

static unsigned long bzero_calls;

static void kstr_bzero(char *s, size_t n)
{
    bzero_calls++;
    while (n-- != 0)
        *s++ = 0;
}

static void kstr_bcopy(const char *src, char *dst, size_t n)
{
    while (n-- != 0)
        *dst++ = *src++;
}

/* Rename what kstring.c defines and calls, so the host C library's own
 * memset/memmove/memcmp/bzero/bcopy stay the reference. */
#define bzero   kstr_bzero_k
#define bcopy   kstr_bcopy_k
#define memset  kstr_memset
#define memmove kstr_memmove
#define memcmp  kstr_memcmp
#define strncpy kstr_strncpy

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wold-style-definition"
#pragma GCC diagnostic ignored "-Wpedantic"
#define ODFS_PLATFORM_AMIX_KERNEL 1
#define CDFS_MODULE 1
#include "../../platform/amix-kernel/kstring.c"
#undef ODFS_PLATFORM_AMIX_KERNEL
#undef CDFS_MODULE
#pragma GCC diagnostic pop

#undef bzero
#undef bcopy
#undef memset
#undef memmove
#undef memcmp
#undef strncpy

/* The K&R names kstring.c calls resolve to the counting stand-ins. */
void kstr_bzero_k(caddr_t s, size_t n);
void kstr_bcopy_k(caddr_t src, caddr_t dst, size_t n);
void kstr_bzero_k(caddr_t s, size_t n) { kstr_bzero(s, n); }
void kstr_bcopy_k(caddr_t src, caddr_t dst, size_t n) { kstr_bcopy(src, dst, n); }

#define GUARD 16
#define MAXLEN 1300                 /* past rr_info_t's 1 184 bytes */

static unsigned char got[GUARD + 8 + MAXLEN + GUARD];
static unsigned char want[GUARD + 8 + MAXLEN + GUARD];

static void fill_pattern(unsigned char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        b[i] = (unsigned char)(i * 37u + 11u);
}

static int memset_matches(size_t head, size_t len, int c)
{
    void *ret;

    fill_pattern(got, sizeof(got));
    fill_pattern(want, sizeof(want));
    ret = kstr_memset(got + GUARD + head, c, len);
    memset(want + GUARD + head, c, len);
    return ret == got + GUARD + head && memcmp(got, want, sizeof(got)) == 0;
}

TEST(memset_matches_libc_nonzero)
{
    static const int values[] = { 1, 0x5a, 0x80, 0xff, 0x1a5, -1 };
    size_t head, len, v;

    for (v = 0; v < sizeof(values) / sizeof(values[0]); v++)
        for (head = 0; head < 8; head++)
            for (len = 0; len <= 64; len++)
                ASSERT(memset_matches(head, len, values[v]));
    for (v = 0; v < sizeof(values) / sizeof(values[0]); v++)
        for (head = 0; head < 8; head++)
            for (len = 1180; len <= MAXLEN; len++)
                ASSERT(memset_matches(head, len, values[v]));
}

TEST(memset_nonzero_does_not_call_bzero)
{
    bzero_calls = 0;
    ASSERT(memset_matches(3, 1184, 0x20));
    ASSERT_EQ(bzero_calls, 0);
}

TEST(memset_zero_goes_to_bzero)
{
    size_t head, len;

    for (head = 0; head < 8; head++) {
        for (len = 1; len <= MAXLEN; len += 7) {
            bzero_calls = 0;
            ASSERT(memset_matches(head, len, 0));
            ASSERT_EQ(bzero_calls, 1);
        }
    }
    /* only the low byte of c counts: 0x100 is a clear */
    bzero_calls = 0;
    ASSERT(memset_matches(1, 1184, 0x100));
    ASSERT_EQ(bzero_calls, 1);
}

TEST(memset_zero_length_touches_nothing)
{
    bzero_calls = 0;
    ASSERT(memset_matches(0, 0, 0));
    ASSERT(memset_matches(5, 0, 0x33));
    ASSERT_EQ(bzero_calls, 0);
}

TEST(memmove_matches_libc_on_overlap)
{
    static unsigned char a[256], b[256];
    size_t src, dst, len;

    for (src = 0; src < 24; src++) {
        for (dst = 0; dst < 24; dst++) {
            for (len = 0; len <= 40; len++) {
                fill_pattern(a, sizeof(a));
                fill_pattern(b, sizeof(b));
                ASSERT(kstr_memmove(a + dst, a + src, len) == a + dst);
                memmove(b + dst, b + src, len);
                ASSERT(memcmp(a, b, sizeof(a)) == 0);
            }
        }
    }
}

TEST(memcmp_sign_and_extent)
{
    unsigned char x[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned char y[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

    ASSERT_EQ(kstr_memcmp(x, y, 8), 0);
    ASSERT_EQ(kstr_memcmp(x, y, 0), 0);
    y[5] = 0x80;
    ASSERT(kstr_memcmp(x, y, 8) < 0);
    ASSERT(kstr_memcmp(y, x, 8) > 0);
    ASSERT_EQ(kstr_memcmp(x, y, 5), 0);
}

TEST(strncpy_pads_with_nuls)
{
    char d[8];

    memset(d, 'x', sizeof(d));
    ASSERT(kstr_strncpy(d, "ab", sizeof(d)) == d);
    ASSERT(d[0] == 'a' && d[1] == 'b');
    ASSERT(d[2] == 0 && d[7] == 0);
    memset(d, 'x', sizeof(d));
    kstr_strncpy(d, "abcdefghij", 4);
    ASSERT(memcmp(d, "abcdxxxx", 8) == 0);
}

TEST_MAIN()
