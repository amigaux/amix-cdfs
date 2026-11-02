/*
 * kstring.c — the mem primitives the AMIX kernel does NOT already export.
 *
 * The portable core/backends — and GCC's own lowering of aggregate struct
 * copies/zero-inits — reference memcpy/memmove/memset/memcmp/strlen. But of
 * those the kernel's ml/exp module ALREADY exports GLOBAL memcpy and strlen,
 * so cdfs must NOT define them: a second definition makes the whole-kernel
 * `ld -o unix -r` fail with "multiple definition of 'memcpy'"/"'strlen'".
 * memcpy and strlen are therefore left as undefined externs (declared in
 * compat/string.h) and resolve to the kernel's ml/exp copies at the final
 * link — the standard ABI every other kernel module relies on.
 *
 * The kernel exports NEITHER memmove NOR memset, and its only memcmp lives in
 * netinet as a static/local symbol (no clash), so cdfs supplies exactly those
 * three here, layered on the kernel's bcopy and bzero where that is safe.
 *
 * WHAT THE KERNEL'S bcopy REALLY IS — load-bearing for memmove below. bcopy is
 * NOT overlap-safe. It tail-jumps straight into mcpy, which copies strictly
 * ASCENDING (`moveb %a0@+,%a1@+` for the alignment head and tail, `movel
 * %a0@+,%a1@+` for the long-word body) — a plain forward memcpy, and in fact
 * the kernel's own memcpy is just a second entry point into the same mcpy with
 * the arguments swapped. So bcopy may stand in for memmove only when a forward
 * copy is safe; where it is not, memmove must copy backward itself.
 *
 * MUST be compiled with -fno-builtin (see the Makefile per-object override):
 * otherwise GCC recognizes the byte loops below as the very memmove/memset/
 * memcmp idioms and rewrites them into self-calls (infinite recursion) — the
 * same freestanding trap the OS4 handler's freestanding.o guards against.
 *
 * The kernel routines are spelled K&R (no prototype), matching the period
 * kernel's own <sys/systm.h> declarations, and <sys/systm.h> is deliberately
 * NOT included: it would drag in non-const strcmp/strncpy prototypes that clash
 * with the compiler built-ins. bcopy's argument order is (src, dst, n) — the
 * OPPOSITE of memcpy(dst, src, n); the wrappers swap accordingly.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#if defined(ODFS_PLATFORM_AMIX_KERNEL)

#include <sys/types.h>   /* size_t, caddr_t */

/* memset's fill word: 32 bits on m68k and on the hosts the unit test runs
 * on (tests/unit/test_kstring.c); emits nothing. */
typedef char kstring_word_check[sizeof(unsigned int) == 4 ? 1 : -1];

/* Kernel primitives, declared K&R (whatever the kernel provides). */
extern void bcopy(/* caddr_t src, caddr_t dst, size_t n */);
extern void bzero(/* caddr_t s, size_t n */);

/*
 * memmove — overlap-safe, which bcopy is NOT (see the header: bcopy/mcpy copy
 * strictly forward). cdfs itself has no memmove call site today, but this is a
 * GLOBAL symbol in the whole-kernel `ld -r` link, so ANY kernel code that
 * references memmove binds to this body. It must therefore be correct on its
 * own terms, not merely correct for the callers cdfs happens to have.
 *
 *   dst <  src              — forward is safe even when the regions overlap:
 *                             each source byte is read before the write that
 *                             would clobber it.
 *   dst >= src + n          — the regions are disjoint; forward is safe.
 *   src <  dst <  src + n   — DESTRUCTIVE overlap. A forward copy would
 *                             overwrite source bytes it has not yet read, so
 *                             copy backward instead.
 *
 * The two safe cases delegate to the kernel's bcopy for its long-word loop; the
 * destructive case is a backward byte loop here.
 */
void *memmove(dst, src, n)
    void *dst;
    const void *src;
    size_t n;
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t               i;

    if (n == 0 || d == s)
        return dst;

    if (d < s || d >= s + n) {
        bcopy((caddr_t)src, (caddr_t)dst, n);
        return dst;
    }

    /* Destructive overlap: descend, so every byte is read before it is
     * overwritten. -fno-builtin keeps GCC from turning this back into a
     * memmove self-call. */
    i = n;
    while (i-- > 0)
        d[i] = s[i];

    return dst;
}

void *memset(s, c, n)
    void *s;
    int c;
    size_t n;
{
    unsigned char *p = (unsigned char *)s;
    unsigned char  v = (unsigned char)c;
    unsigned int  *q;
    unsigned int   w;
    size_t         words;

    if (n == 0)
        return s;

    /*
     * c == 0, which is nearly every call (the Rock Ridge parser clears a
     * 1 184-byte rr_info_t per directory record, and GCC lowers struct
     * zero-inits to memset): the kernel's own bzero, a DDI minor-0 name.
     * It aligns the destination with a byte head, clears the body a long
     * word at a time and finishes with a byte tail; its length test is
     * signed, which no cdfs size comes near.
     *
     * HISTORY, corrected — do not reintroduce the old claim. This body was
     * once a byte loop for every c, justified by "the kernel's bzero does not
     * reliably clear a large region": a ~1.1 KB rr_info_t kept garbage in its
     * tail fields (offset > 1024), which made the ISO readdir skip every
     * child as a Rock Ridge relocation. That diagnosis was WRONG. bzero is a
     * correct clear; the culprit was the Z3660 firmware's DMA cache-coherence
     * bug — stale lines, not an unclear — fixed in Z3660 0a4c064.
     */
    if (v == 0) {
        bzero((caddr_t)s, n);
        return s;
    }

    /* Any other c: the same shape as bzero. Byte head up to a long-word
     * boundary, long words (four per pass), byte tail. */
    while (n != 0 && ((unsigned long)p & 3) != 0) {
        *p++ = v;
        n--;
    }
    if (n >= 4) {
        w = v;
        w |= w << 8;
        w |= w << 16;
        q = (unsigned int *)p;
        words = n >> 2;
        n &= 3;
        while (words >= 4) {
            *q++ = w;
            *q++ = w;
            *q++ = w;
            *q++ = w;
            words -= 4;
        }
        while (words != 0) {
            *q++ = w;
            words--;
        }
        p = (unsigned char *)q;
    }
    while (n != 0) {
        *p++ = v;
        n--;
    }
    return s;
}

int memcmp(a, b, n)
    const void *a;
    const void *b;
    size_t n;
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    size_t               i;

    for (i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

#if defined(CDFS_MODULE)
/*
 * strncpy — the loadable-module form only. The kernel has a strncpy, so the
 * static object leaves it undefined for the kernel link; but it is not one of
 * the names a module may import (the module DDI), so the module carries its
 * own. Standard semantics: copy up to n bytes, then pad with NULs to n.
 */
char *strncpy(dst, src, n)
    char *dst;
    const char *src;
    size_t n;
{
    size_t i;

    for (i = 0; i < n && src[i] != '\0'; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}
#endif /* CDFS_MODULE */

#endif /* ODFS_PLATFORM_AMIX_KERNEL */
