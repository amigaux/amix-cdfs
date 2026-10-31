/*
 * odfs/alloc.h — small allocation wrapper for shared code
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_ALLOC_H
#define ODFS_ALLOC_H

#include "odfs/config.h"

#include <stddef.h>

#if defined(ODFS_PLATFORM_AMIX)
/*
 * AMIX (SVR4/m68k) userspace uses the plain libc malloc/free path in the
 * #else branch below — never the Amiga AllocVec/FreeVec path. This holds
 * because ODFS_PLATFORM_AMIGA is 0 on AMIX; the guard here only documents
 * that deliberate choice and emits nothing.
 */
#endif

#if defined(ODFS_PLATFORM_AMIX_KERNEL)

/*
 * AMIX in-kernel context: no libc. Allocate through the SVR4 kernel memory
 * allocator (<sys/kmem.h>). The mismatch this arm has to bridge: the core's
 * odfs_free(ptr) is handed no size, but SVR4 kmem_free(cp, size) REQUIRES
 * the original length. So odfs_malloc/odfs_calloc over-allocate an aligned
 * size_t header, stash the total (header + payload) length there, and hand
 * back the payload pointer; odfs_free reads the length back for kmem_free.
 * Every request is KM_NOSLEEP (see ODFS_KMEM_FLAGS below): every caller
 * turns NULL into ODFS_ERR_NOMEM, so memory pressure fails a mount or a
 * read with ENOMEM instead of putting it to sleep.
 */
#include <sys/types.h>   /* size_t + _VOID, needed by <sys/kmem.h> */
#include <sys/kmem.h>    /* kmem_alloc/kmem_zalloc/kmem_free, KM_[NO]SLEEP */

/*
 * Header size: one size_t, rounded up to 8-byte alignment so the returned
 * payload keeps the 8-byte alignment the core's 64-bit fields expect. kmem
 * hands back at least word-aligned memory, so base + 8 stays 8-aligned.
 */
#define ODFS_KMEM_HDR ((size_t)8)

/*
 * Stock SVR4 kmem serves a request over 4096 bytes (MAXABIG) as "oversize":
 * sptalloc() of that many contiguous kernel-VA pages. When no such run is
 * free, KM_SLEEP sleeps at PZERO on Km_OsizeWanted until somebody frees an
 * oversize block at least as large, which may never happen: the caller
 * hangs unkillably (docs/amix-kmem-oversize.md). Whether a pool-sized
 * KM_SLEEP request can wait the same way when its pool cannot grow is not
 * established, so pool-sized requests are KM_NOSLEEP too: a mount or a
 * read under memory pressure returns ENOMEM rather than wait.
 */
#define ODFS_KMEM_MAXABIG ((size_t)4096)
#define ODFS_KMEM_FLAGS(total) KM_NOSLEEP

static inline void *odfs_malloc(size_t size)
{
    char   *base;
    size_t  total;

    if (size == 0)
        size = 1;
    total = size + ODFS_KMEM_HDR;
    base = (char *)kmem_alloc(total, ODFS_KMEM_FLAGS(total));
    if (base == 0)
        return 0;
    *(size_t *)base = total;
    return (void *)(base + ODFS_KMEM_HDR);
}

static inline void *odfs_calloc(size_t count, size_t size)
{
    char   *base;
    size_t  total;

    if (size != 0 && count > ((size_t)-1) / size)
        return 0;

    total = count * size;
    if (total == 0)
        total = 1;
    total += ODFS_KMEM_HDR;

    base = (char *)kmem_zalloc(total, ODFS_KMEM_FLAGS(total));
    if (base == 0)
        return 0;
    *(size_t *)base = total;
    return (void *)(base + ODFS_KMEM_HDR);
}

static inline void odfs_free(void *ptr)
{
    char *base;

    if (ptr == 0)
        return;
    base = (char *)ptr - ODFS_KMEM_HDR;
    kmem_free((void *)base, *(size_t *)base);
}

#elif ODFS_PLATFORM_AMIGA

#include "amiga_target_compat.h"

#include <exec/memory.h>
#include <exec/types.h>
#if ODFS_AMIGA_OS4
#include <exec/exectags.h>
#include <utility/tagitem.h>
#endif
#include <proto/exec.h>

static inline void *odfs_malloc(size_t size)
{
    if (size == 0)
        size = 1;
#if ODFS_AMIGA_OS4
    return AllocVecTags((ULONG)size, AVT_Type, MEMF_SHARED, TAG_END);
#else
    return AllocVec((ULONG)size, MEMF_PUBLIC);
#endif
}

static inline void *odfs_calloc(size_t count, size_t size)
{
    size_t total;

    if (size != 0 && count > ((size_t)-1) / size)
        return NULL;

    total = count * size;
    if (total == 0)
        total = 1;

#if ODFS_AMIGA_OS4
    return AllocVecTags((ULONG)total,
                        AVT_Type, MEMF_SHARED,
                        AVT_ClearWithValue, 0,
                        TAG_END);
#else
    return AllocVec((ULONG)total, MEMF_PUBLIC | MEMF_CLEAR);
#endif
}

static inline void odfs_free(void *ptr)
{
    if (ptr)
        FreeVec(ptr);
}

#else

#include <stdlib.h>

static inline void *odfs_malloc(size_t size)
{
    return malloc(size);
}

static inline void *odfs_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}

static inline void odfs_free(void *ptr)
{
    free(ptr);
}

#endif

/*
 * Aligned allocation for buffers a media backend hands to hardware.
 *
 * Sector buffers (the block cache's slab, its per-entry fallback, the
 * read-ahead staging buffer, the multi-session PVD scan buffer) are DMA
 * TARGETS on real-hardware targets: the Amiga handler's exec devices and the
 * AMIX in-kernel SCSI backend both point a bus-master engine at them. Plain
 * odfs_malloc gives no useful alignment — on the AMIX kernel arm it returns
 * kmem payloads 8 bytes past a page boundary, so a sector-sized transfer
 * CROSSES the page boundary, and because large SVR4 kmem allocations are
 * virtually contiguous but PHYSICALLY SCATTERED (sptalloc/segkmem map
 * arbitrary page_get frames), the tail of the transfer lands in an unrelated
 * physical page — a wild write into whatever the frame allocator's neighbour
 * happens to be. Aligning every sector buffer to the sector size (== the AMIX
 * page size, 2048) makes each sector occupy exactly one page, so a bounded
 * transfer can never leave its own frame.
 *
 * Generic over odfs_malloc/odfs_free: over-allocate by align + one pointer,
 * round the payload up to `align`, and stash the raw pointer immediately
 * below the payload for odfs_free_aligned. `align` must be a power of two.
 * (Pointer<->size_t round-tripping is exact on every supported target:
 * flat 32-bit m68k and 64-bit hosts where size_t matches the pointer width.)
 */
static inline void *odfs_malloc_aligned(size_t size, size_t align)
{
    char   *raw;
    char   *payload;
    size_t  addr;

    if (align == 0 || (align & (align - 1)) != 0)
        return 0;
    if (size > ((size_t)-1) - align - sizeof(void *))
        return 0;

    raw = (char *)odfs_malloc(size + align + sizeof(void *));
    if (raw == 0)
        return 0;

    addr = (size_t)(raw + sizeof(void *));
    addr = (addr + (align - 1)) & ~(align - 1);
    payload = (char *)addr;
    ((void **)(void *)payload)[-1] = (void *)raw;
    return (void *)payload;
}

static inline void odfs_free_aligned(void *ptr)
{
    if (ptr)
        odfs_free(((void **)ptr)[-1]);
}

#endif /* ODFS_ALLOC_H */
