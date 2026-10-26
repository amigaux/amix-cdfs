/*
 * cdfs_mod.h — the names only the loadable-module form of cdfs has
 * (built with -DCDFS_MODULE; see README.md, "The loadable module").
 *
 * The static kernel object finds its vfssw row by name and checks for a
 * drive mounted twice with vfs_devsearch(). A module may import neither
 * vfssw/nfstype nor vfs_devsearch, so the module form gets its row index
 * from vfs_register(), which runs cdfsinit(row, index) once, and keeps its
 * own table of mounted drives. Bodies in cdfs_modlocal.c.
 *
 * <sys/types.h> (dev_t) must come first.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ODFS_AMIX_KERNEL_CDFS_MOD_H
#define ODFS_AMIX_KERNEL_CDFS_MOD_H

#if defined(CDFS_MODULE)

struct vfssw;

/* makedevice(major, 0), the major from getudev(); latched by cdfsinit(). */
extern dev_t cdfs_devbase;

/* The fstype index (cdfs_vfsops.c), set by cdfsinit() from its index. */
extern int cdfs_fstype;

/* The vsw_init routine vfs_register() runs once: (the row, its index). */
int cdfsinit(struct vfssw *vswp, int idx);

/* Claim a drive for a mount: 0, or nonzero when it is mounted already. */
int cdfs_drive_claim(dev_t dev);

/* Give a claimed drive back: a failed mount, or an unmount. */
void cdfs_drive_release(dev_t dev);

#endif /* CDFS_MODULE */

#endif /* ODFS_AMIX_KERNEL_CDFS_MOD_H */
