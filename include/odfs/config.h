/*
 * odfs/config.h — compile-time feature flags and build profiles
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_CONFIG_H
#define ODFS_CONFIG_H

/*
 * Build profiles:
 *   ODFS_PROFILE_FULL  — all features enabled (default)
 *   ODFS_PROFILE_ROM   — minimal ROM-capable subset
 *
 * Set exactly one profile, or configure individual flags below.
 */

#if !defined(ODFS_PROFILE_FULL) && !defined(ODFS_PROFILE_ROM)
#define ODFS_PROFILE_FULL
#endif

/* ---------- backend feature flags ---------- */
/* All flags can be overridden from command line (-DODFS_FEATURE_xxx=0) */

#ifndef ODFS_FEATURE_ISO9660
  #define ODFS_FEATURE_ISO9660      1
#endif
#ifndef ODFS_FEATURE_ROCK_RIDGE
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_ROCK_RIDGE 1
  #else
    #define ODFS_FEATURE_ROCK_RIDGE 1
  #endif
#endif
#ifndef ODFS_FEATURE_JOLIET
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_JOLIET     1
  #else
    #define ODFS_FEATURE_JOLIET     1
  #endif
#endif
#ifndef ODFS_FEATURE_HIGH_SIERRA
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_HIGH_SIERRA 0
  #else
    #define ODFS_FEATURE_HIGH_SIERRA 1
  #endif
#endif
#ifndef ODFS_FEATURE_MULTISESSION
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_MULTISESSION 1
  #else
    #define ODFS_FEATURE_MULTISESSION 1
  #endif
#endif
#ifndef ODFS_FEATURE_UDF
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_UDF        0
  #else
    #define ODFS_FEATURE_UDF        1
  #endif
#endif
#ifndef ODFS_FEATURE_HFS
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_HFS        0
  #else
    #define ODFS_FEATURE_HFS        1
  #endif
#endif
#ifndef ODFS_FEATURE_HFSPLUS
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_HFSPLUS    0
  #else
    #define ODFS_FEATURE_HFSPLUS    1
  #endif
#endif
#ifndef ODFS_FEATURE_CDDA
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_CDDA       0
  #else
    #define ODFS_FEATURE_CDDA       1
  #endif
#endif

/* ---------- cache feature flags ---------- */

#ifndef ODFS_FEATURE_CACHE_BLOCK
  #define ODFS_FEATURE_CACHE_BLOCK    1
#endif
#ifndef ODFS_FEATURE_CACHE_META
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_CACHE_META   0
  #else
    #define ODFS_FEATURE_CACHE_META   1
  #endif
#endif
/* block-cache read-ahead: clustered miss reads through a staging buffer
 * (core/cache_block.c) */
#ifndef ODFS_FEATURE_CACHE_STREAM
  #ifdef ODFS_PROFILE_ROM
    #define ODFS_FEATURE_CACHE_STREAM 0
  #elif defined(ODFS_PLATFORM_AMIX_KERNEL)
    /* off: see the AMIX in-kernel arm below */
    #define ODFS_FEATURE_CACHE_STREAM 0
  #else
    #define ODFS_FEATURE_CACHE_STREAM 1
  #endif
#endif

/* ---------- logging feature flags ---------- */

/* ODFS_FEATURE_LOG can be overridden from command line (-DODFS_FEATURE_LOG=0) */
#ifndef ODFS_FEATURE_LOG
#ifdef ODFS_PROFILE_ROM
  #define ODFS_FEATURE_LOG            1
#else
  #define ODFS_FEATURE_LOG            1
#endif
#endif

#ifdef ODFS_PROFILE_ROM
  #define ODFS_FEATURE_LOG_SERIAL     1
  #define ODFS_FEATURE_LOG_KPRINTF    0
  #define ODFS_FEATURE_LOG_RINGBUF    0
  #define ODFS_LOG_MAX_LEVEL          ODFS_LOG_WARN
#else
  #define ODFS_FEATURE_LOG_SERIAL     1
  #define ODFS_FEATURE_LOG_KPRINTF    1
  #define ODFS_FEATURE_LOG_RINGBUF    1
  #define ODFS_LOG_MAX_LEVEL          ODFS_LOG_TRACE
#endif

/* ---------- charset feature flags ---------- */

#ifdef ODFS_PROFILE_ROM
  #define ODFS_FEATURE_CHARSET_BUILTIN   1
  #define ODFS_FEATURE_CHARSET_EXTERNAL  0
#else
  #define ODFS_FEATURE_CHARSET_BUILTIN   1
  #define ODFS_FEATURE_CHARSET_EXTERNAL  1
#endif

/* ---------- cache sizing defaults ---------- */

#ifdef ODFS_PROFILE_ROM
  #define ODFS_BLOCK_CACHE_SIZE     16
  #define ODFS_META_CACHE_KIB        0
#else
  #define ODFS_BLOCK_CACHE_SIZE    128
  /* parsed-directory cache budget in KiB (0 disables) */
  #define ODFS_META_CACHE_KIB       96
#endif

/* ---------- platform ---------- */

#if defined(AMIGA) || defined(__amigaos__)
  #define ODFS_PLATFORM_AMIGA 1
#else
  #define ODFS_PLATFORM_AMIGA 0
#endif

#if defined(ODFS_PLATFORM_AMIX)
  /*
   * AMIX (SVR4/m68k) userspace. The sysroot ships no <stdint.h> or
   * <inttypes.h>, so pull the fixed-width types + PRI* macros in here
   * (matching the tree's practice of getting them from an include).
   */
  #include "odfs/compat_stdint.h"
  /* AMIX is its own platform, not the host template. */
  #ifndef ODFS_PLATFORM_HOST
    #define ODFS_PLATFORM_HOST 0
  #endif
#endif

#if defined(ODFS_PLATFORM_AMIX_KERNEL)
  /*
   * AMIX in-kernel context: no libc, no userspace syscalls, no <stdio.h>.
   * Like the userspace AMIX arm above, the sysroot ships no <stdint.h>, so
   * pull the fixed-width types in here; and like it, AMIX is its own
   * platform, never the host template — force ODFS_PLATFORM_HOST 0 so the
   * host default below cannot claim this build.
   */
  #include "odfs/compat_stdint.h"
  #ifndef ODFS_PLATFORM_HOST
    #define ODFS_PLATFORM_HOST 0
  #endif
  /*
   * The kernel may run on a 68060, which traps 32x32->64 multiplies and
   * has no emulation package in kernel mode; see odfs/arith.h.
   */
  #ifndef ODFS_NO_MUL64
    #define ODFS_NO_MUL64 1
  #endif
  /*
   * No per-sector fallback when the block cache's slab cannot be had: the
   * slab fails only when the kernel map has no free run long enough, and
   * one oversize buffer per sector would then take more of the same map.
   * The mount fails with ENOMEM instead (core/cache_block.c).
   */
  #ifndef ODFS_CACHE_SLAB_FALLBACK
    #define ODFS_CACHE_SLAB_FALLBACK 0
  #endif
  /*
   * ODFS_FEATURE_CACHE_STREAM is 0 here (set in the cache flags above,
   * which come first): the kernel media sends one sector per SCSI command,
   * so an 8-sector read-ahead is eight commands either way, and its
   * staging buffer would be one more oversize kmem request per mount.
   */
#endif

/* host builds for testing */
#ifndef ODFS_PLATFORM_HOST
  #if !ODFS_PLATFORM_AMIGA
    #define ODFS_PLATFORM_HOST 1
  #else
    #define ODFS_PLATFORM_HOST 0
  #endif
#endif

#endif /* ODFS_CONFIG_H */
