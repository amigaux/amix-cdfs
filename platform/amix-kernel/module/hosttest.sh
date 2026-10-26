#!/bin/sh
# hosttest.sh -- host test of the cdfs module's load path, without a box.
#
# Compiles the shipped cdfs_mod.c (the module wrapper and _load) and
# cdfs_modlocal.c (the module form's cdfsinit and mounted-drive table) with
# the host cc, against stub headers for the kernel's sys/moddefs.h,
# sys/amixddi.h and sys/sysmacros.h (the kit's makedevice and L_MAXMAJ). The
# test provides what they call: vfs_register (records the call and, as the
# module core does, runs init(row, index) once), getudev (a counter, or -1),
# cdfs_vfsops and cdfs_fstype. It checks:
#   - the wrapper: a _load, no _unload (pinned), no _halt;
#   - _load registers keys' fs= with &cdfs_vfsops and flag 0, and returns
#     vfs_register's result (EEXIST, ENOSPC) unchanged;
#   - init takes the index as the fstype and latches one getudev() major,
#     L_MAXMAJ when getudev() has none left;
#   - the drive table: a drive is claimed once, given back, and 32 fit.
#
#   usage: sh platform/amix-kernel/module/hosttest.sh     (HOSTCC overrides cc)
#
# SPDX-License-Identifier: BSD-2-Clause
set -e
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/.." && pwd)
cc=${HOSTCC:-cc}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/inc/sys"

fs=$(sed -n 's/^fs=//p' "$here/keys")
[ -n "$fs" ] || { echo "hosttest: keys has no fs=" >&2; exit 1; }

cat > "$tmp/inc/sys/types.h" <<'H'
#include_next <sys/types.h>
H
cat > "$tmp/inc/sys/sysmacros.h" <<'H'
/* as the kit's sys/sysmacros.h */
#define L_BITSMINOR	18
#define L_MAXMAJ	0xff
#define L_MAXMIN	0x3ffff
#define makedevice(x,y)	(unsigned long)(((x)<<L_BITSMINOR) | ((y)&L_MAXMIN))
H
cat > "$tmp/inc/sys/moddefs.h" <<'H'
struct td_wrapper { int (*load)(); int (*unload)(); void (*halt)(); char *desc; };
#define MOD_MISC_WRAPPER(p, l, u, d) struct td_wrapper p##_wrapper = { l, u, 0, d }
H
cat > "$tmp/inc/sys/amixddi.h" <<'H'
struct vfsops;
int vfs_register(char *, int (*)(), struct vfsops *, long);
H

cat > "$tmp/t.c" <<'C'
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysmacros.h>

struct td_wrapper { int (*load)(); int (*unload)(); void (*halt)(); char *desc; };
extern struct td_wrapper cdfs_wrapper;

struct vfsops { int td_dummy; };
struct vfssw { char *vsw_name; };
struct vfsops cdfs_vfsops;
int cdfs_fstype = 0;

extern dev_t cdfs_devbase;
int cdfs_drive_claim(dev_t);
void cdfs_drive_release(dev_t);

static int td_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); td_fail++; } } while (0)

static int td_udev = 40, td_getudev_calls;
int getudev(void) { td_getudev_calls++; return td_udev; }

static int td_reg_calls, td_reg_rc, td_idx = 13;
static char td_name[32];
static struct vfsops *td_ops;
static long td_flag;
static struct vfssw td_row;
int vfs_register(char *name, int (*init)(), struct vfsops *ops, long flag)
{
	td_reg_calls++;
	strncpy(td_name, name, sizeof td_name - 1);
	td_ops = ops;
	td_flag = flag;
	if (td_reg_rc != 0)
		return td_reg_rc;
	td_row.vsw_name = name;
	if (init != 0)
		(void)(*init)(&td_row, td_idx);	/* as vfsinit: (row, index), result unread */
	return 0;
}

int main(void)
{
	dev_t d[33];
	int i;

	/* the wrapper */
	CHECK(cdfs_wrapper.load != 0);
	CHECK(cdfs_wrapper.unload == 0);
	CHECK(cdfs_wrapper.halt == 0);
	CHECK(cdfs_wrapper.desc != 0 && strstr(cdfs_wrapper.desc, "cdfs") != 0);

	/* _load: one vfs_register with fs=, &cdfs_vfsops, flag 0; init(row, 13) */
	CHECK((*cdfs_wrapper.load)((char *)0) == 0);
	CHECK(td_reg_calls == 1);
	CHECK(strcmp(td_name, TD_FS) == 0);
	CHECK(td_ops == &cdfs_vfsops);
	CHECK(td_flag == 0);
	CHECK(cdfs_fstype == 13);
	CHECK(td_getudev_calls == 1);
	CHECK(cdfs_devbase == (dev_t)makedevice(40, 0));

	/* refused: the errno passes through, init does not run */
	td_reg_rc = EEXIST;
	td_idx = 5;
	CHECK((*cdfs_wrapper.load)("x=1") == EEXIST);
	td_reg_rc = ENOSPC;
	CHECK((*cdfs_wrapper.load)((char *)0) == ENOSPC);
	CHECK(cdfs_fstype == 13);

	/* init again: a new index, the major stays latched */
	td_reg_rc = 0;
	CHECK((*cdfs_wrapper.load)((char *)0) == 0);
	CHECK(cdfs_fstype == 5);
	CHECK(td_getudev_calls == 1);

	/* no major left: L_MAXMAJ, as the static object */
	cdfs_devbase = 0;
	td_udev = -1;
	CHECK((*cdfs_wrapper.load)((char *)0) == 0);
	CHECK(cdfs_devbase == (dev_t)makedevice(L_MAXMAJ, 0));

	/* the drive table */
	for (i = 0; i < 33; i++)
		d[i] = cdfs_devbase | (dev_t)(((i / 8) << 8) | (i % 8));
	CHECK(cdfs_drive_claim(d[2]) == 0);
	CHECK(cdfs_drive_claim(d[2]) != 0);		/* mounted: EBUSY */
	CHECK(cdfs_drive_claim(d[3]) == 0);
	cdfs_drive_release(d[2]);
	CHECK(cdfs_drive_claim(d[2]) == 0);		/* given back */
	cdfs_drive_release(d[9]);			/* not claimed: nothing */
	for (i = 0; i < 32; i++)
		if (i != 2 && i != 3)
			CHECK(cdfs_drive_claim(d[i]) == 0);
	d[32] = cdfs_devbase | (dev_t)((4 << 8) | 0);
	CHECK(cdfs_drive_claim(d[32]) != 0);		/* 32 slots, all taken */
	cdfs_drive_release(d[31]);
	CHECK(cdfs_drive_claim(d[32]) == 0);
	CHECK(cdfs_drive_claim(d[31]) != 0);

	if (td_fail) {
		printf("hosttest: %d failure(s)\n", td_fail);
		return 1;
	}
	printf("hosttest: cdfs module load path ok (fs=%s)\n", TD_FS);
	return 0;
}
C

$cc -std=gnu89 -w -I"$tmp/inc" -c "$src/cdfs_mod.c" -o "$tmp/cdfs_mod.o"
$cc -std=gnu89 -Wall -Wno-unused-parameter -I"$tmp/inc" -I"$src" \
	-DODFS_PLATFORM_AMIX_KERNEL -DCDFS_MODULE \
	-c "$src/cdfs_modlocal.c" -o "$tmp/cdfs_modlocal.o"
$cc -std=gnu99 -Wall -I"$tmp/inc" -DTD_FS="\"$fs\"" \
	"$tmp/t.c" "$tmp/cdfs_mod.o" "$tmp/cdfs_modlocal.o" -o "$tmp/t"
"$tmp/t"
