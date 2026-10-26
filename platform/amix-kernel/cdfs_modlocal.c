/*
 * cdfs_modlocal.c — what the loadable-module form of cdfs does in place of
 * the static object's vfssw search and vfs_devsearch() (cdfs_mod.h).
 *
 * Built only with -DCDFS_MODULE (the module's object, README.md "The
 * loadable module"); the static kernel object does not contain it.
 *
 * C89 for the period cross compiler (GCC 2.7.2.3).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#if defined(ODFS_PLATFORM_AMIX_KERNEL) && defined(CDFS_MODULE)

#include <sys/types.h>
#include <sys/sysmacros.h>     /* makedevice(), L_MAXMAJ */
#include "cdfs_mod.h"

/* The kernel's pseudo-major allocator (cdfs_vfsops.c says why). */
extern int getudev(/* void */);

dev_t cdfs_devbase = (dev_t)0;

/*
 * cdfsinit — run once by vfs_register() from the module's _load, with the
 * row it filled and that row's index, as the stock vfsinit runs each row's
 * init. vfs_register has already put &cdfs_vfsops in the row, so the row is
 * not touched here; the index is the fstype every mount stamps into
 * vfs_fstype and vfs_fsid.
 *
 * It latches the device major exactly as the static cdfsinit does: one
 * getudev() major for the life of the boot, L_MAXMAJ if they ran out.
 *
 * Context: the module's _load. For a boot-class load that is process context
 * at the first sysinit entry; for a root-class load it is the core's root
 * pass, at level 7 with the clock stopped. Neither getudev() nor this routine
 * sleeps or allocates, so it is safe in both.
 */
int cdfsinit(struct vfssw *vswp, int idx)
{
    int udev;

    (void)vswp;

    if (cdfs_devbase == (dev_t)0) {
        udev = getudev();
        if (udev == -1)
            udev = (int)L_MAXMAJ;
        cdfs_devbase = (dev_t)makedevice((unsigned int)udev, 0);
    }
    cdfs_fstype = idx;
    return 0;
}

/*
 * The mounted drives. Only a cdfs vfs carries the cdfs pseudo-major, so
 * "is a cdfs vfs mounted on this (card,unit) dev" is what vfs_devsearch()
 * answered for the static object; this table answers it for the module.
 *
 * At most SDCARDS x SDUNITS = 4 x 8 drives exist, so 32 slots never fill.
 * 0 marks a free slot: a mounted dev always has a nonzero major (getudev's,
 * or L_MAXMAJ). The claim is made in cdfs_mount's guard and released on
 * every failed mount and at unmount. Neither routine sleeps, and the kernel
 * does not preempt, so claim-and-test is one step: unlike the static check,
 * a second mount of a drive that is still being mounted is refused too.
 */
#define CDFS_MAXDRIVES 32

static dev_t cdfs_drives[CDFS_MAXDRIVES];

int cdfs_drive_claim(dev_t dev)
{
    int i;
    int slot = -1;

    for (i = 0; i < CDFS_MAXDRIVES; i++) {
        if (cdfs_drives[i] == dev)
            return 1;
        if (cdfs_drives[i] == (dev_t)0 && slot < 0)
            slot = i;
    }
    if (slot < 0)
        return 1;
    cdfs_drives[slot] = dev;
    return 0;
}

void cdfs_drive_release(dev_t dev)
{
    int i;

    for (i = 0; i < CDFS_MAXDRIVES; i++) {
        if (cdfs_drives[i] == dev) {
            cdfs_drives[i] = (dev_t)0;
            return;
        }
    }
}

#endif /* ODFS_PLATFORM_AMIX_KERNEL && CDFS_MODULE */
