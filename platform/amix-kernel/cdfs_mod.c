/*
 * cdfs_mod.c — the module wrapper: cdfs as an Amix loadable filesystem
 * module (README.md, "The loadable module").
 *
 * Compiled by the amix-dlm ABI kit's own rule (K&R: the kit compiles with
 * -traditional) against the kit's headers. Everything else is the
 * -DCDFS_MODULE object the repository's Makefile builds (amix-kernel-module),
 * linked in as the kit's EXTRA.
 *
 * _load registers the filesystem type and does nothing else: no I/O, no
 * sleep, no allocation, so it is safe both at a boot-class load (the first
 * sysinit entry) and in the root pass, should a generation place the module
 * there. vfs_register() runs cdfsinit(row, index) once (cdfs_modlocal.c).
 * There is no _unload (the module is pinned: unload=never) and no _halt
 * (cdfs holds no device state between requests).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "sys/types.h"
#include "sys/moddefs.h"
#include "sys/amixddi.h"

/* The type name: the module's .moddata fs= (module/keys) must equal it. */
#define	CDFS_FSNAME	"cdfs"

struct vfsops;
extern struct vfsops	cdfs_vfsops;	/* cdfs_vfsops.c */
extern int		cdfsinit();	/* cdfs_modlocal.c: (row, index) */

/*
 * params: the manifest's parameter string, or 0. cdfs has no parameters.
 * vfs_register's errno is the load's: EEXIST when the kernel already has a
 * cdfs row (a static cdfs), ENOSPC when the spare rows are taken.
 */
static int
cdfs_load(params)
	char *params;
{
	return vfs_register(CDFS_FSNAME, cdfsinit, &cdfs_vfsops, 0L);
}

MOD_MISC_WRAPPER(cdfs, cdfs_load, 0,
	"cdfs: read-only ISO 9660, Joliet, Rock Ridge, UDF, HFS");
