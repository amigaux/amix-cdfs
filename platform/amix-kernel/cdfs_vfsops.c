/*
 * cdfs_vfsops.c — the SVR4 vfsops vector for the AMIX in-kernel cdfs.
 *
 * Defines cdfs_vfsops POSITIONALLY, in the EXACT slot order of struct vfsops in
 * <sys/vfs.h> (vfs_mount, vfs_unmount, vfs_root, vfs_statvfs, vfs_sync,
 * vfs_vget, vfs_mountroot, vfs_swapvp), plus cdfsinit — the vsw_init routine a
 * later kernel-build step wires into the static vfssw[] table. The op vector is a
 * K&R `int (*)()` array with zero compile-time checking, so the VFS_* call
 * macros in <sys/vfs.h> are the ABI and each function matches them exactly.
 *
 * v1 mount interface (flagged for review): the mount `special` argument is an
 * ASCII "<card><sep><unit>" string — <sep> is any run of non-digits, e.g.
 * "0,2", "0.2", "0:2" — naming the SCSI host-adapter index and target id the
 * in-kernel media backend opens. There is no /dev node yet; a real CD never
 * traverses /dev/dsk on stock AMIX, so addressing by (card,unit) sidesteps that
 * until the device-node story is built out.
 *
 * C89-clean for the period cross compiler (GCC 2.7.2.3).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#if defined(ODFS_PLATFORM_AMIX_KERNEL)

#include "cdfs.h"
#include "amix_kern_media.h"   /* odfs_kern_media_open_scsi() */
#include "odfs/arith.h"

#include <sys/errno.h>
#include <sys/statvfs.h>
#include <sys/cred.h>
#include <sys/kmem.h>          /* kmem_zalloc/kmem_free, KM_NOSLEEP */
#include <sys/mount.h>         /* MS_RDONLY — the mount(2) flag bits */
#include <sys/sysmacros.h>     /* makedevice(), L_MAXMAJ/L_MAXMIN */
#include <string.h>            /* memset, strncpy */
#include "cdfs_mod.h"          /* the module form's own names (CDFS_MODULE) */

/*
 * copyinstr() copies a NUL-terminated string in from user space. Declared K&R
 * (no prototype) rather than pulling <sys/systm.h>, whose non-const strcmp/
 * strncpy prototypes clash with the compiler built-ins — the same reason the
 * media backend declares its kernel externs locally.
 *
 * suser() is the kernel privilege check (returns nonzero for the superuser).
 * mount(2) itself performs NO privilege check — disassembled: no suser call
 * anywhere in it — the kernel delegates that to each filesystem's VFS_MOUNT,
 * and prmount/fdmount/s5mount/nm_mount all open with it. So must cdfs_mount.
 *
 * getudev() hands out unique pseudo device majors in
 * [max(bdevcnt,cdevcnt), 127] from a static counter, -1 when exhausted. It is
 * the kernel's own convention for device-less filesystems — see the
 * synthesized-device-identity block below. (vfs_devsearch() comes prototyped
 * from <sys/vfs.h>.)
 */
extern int copyinstr(/* char *uaddr, char *kaddr, size_t max, size_t *done */);
extern int suser(/* cred_t *cr */);
extern int getudev(/* void */);

/* fstype index, published by cdfsinit(), stamped into vfs_fstype at mount. */
int cdfs_fstype = 0;

/* ---- mount-spec parsing --------------------------------------------- */

/*
 * Parse "<card><sep><unit>" into (card, unit). <sep> is one or more non-digit
 * characters. Returns 0 on success, -1 on malformed input.
 */
static int cdfs_parse_spec(const char *s, unsigned int *card, unsigned int *unit)
{
    unsigned int c = 0;
    unsigned int u = 0;
    int          i = 0;
    int          got;

    got = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        c = c * 10u + (unsigned int)(s[i] - '0');
        i++;
        got = 1;
    }
    if (!got)
        return -1;

    while (s[i] != '\0' && (s[i] < '0' || s[i] > '9'))
        i++;

    got = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        u = u * 10u + (unsigned int)(s[i] - '0');
        i++;
        got = 1;
    }
    if (!got)
        return -1;

    *card = c;
    *unit = u;
    return 0;
}

/* ---- synthesized device identity ------------------------------------ */

/*
 * cdfs has no block-special device: it addresses the drive by (card,unit),
 * because a real CD never traverses /dev/dsk on stock AMIX. The kernel does not
 * care — it walks vfs_dev and vfs_fsid on EVERY mounted vfs regardless:
 *
 *   vfs_devsearch(dev)  linearly scans rootvfs comparing the vfs_dev longs
 *                       (no major-range logic anywhere in it), and is called
 *                       from umount(2) — for a BLOCK-SPECIAL argument only;
 *                       a directory argument resolves by VROOT -> v_vfsp and
 *                       never reads vfs_dev — plus bdevbsize(), and from
 *                       s5mount()/ufs_mount() as their duplicate-device mount
 *                       guard (which cdfs_mount now mirrors).
 *   getvfs(fsid)        scans rootvfs comparing vfs_fsid.val[0..1], and is
 *                       called from the NFS fhtovp()/myfhtovp() paths.
 *
 * Both hand the matched `struct vfs *` straight to code that casts vfs_data to
 * ITS OWN private mount struct — so a vfs_dev/vfs_fsid on our vfs that collides
 * with a real filesystem's is a wild-pointer generator, not a cosmetic defect.
 *
 * And they DO arrive as garbage: mount(2) allocates the vfs with kmem_alloc,
 * not kmem_zalloc, and the VFS_INIT it then runs clears only vfs_next, vfs_op,
 * vfs_flag, vfs_data and vfs_nsubmounts. vfs_dev, vfs_fsid and vfs_bcount are
 * left holding whatever the recycled heap block last contained. Stock s5mount
 * therefore sets all of them, and so must we.
 *
 * WHERE THE MAJOR COMES FROM — the kernel's own convention for device-less
 * filesystems, taken from disassembling this exact kernel: prinit() and
 * fdinit() call getudev(), which hands out a unique major from a static
 * counter starting at max(bdevcnt,cdevcnt) and capped at 127, and latch
 * makedevice(udev, 0) into a global (procdev, fddev, fdrdev) ONCE at vsw-init
 * time; prmount/fdmount then stamp that global into vfs_dev and
 * vfs_fsid=(dev, fstype) on every mount. cdfsinit() does exactly that below.
 * The result is deterministic for the whole boot (never recycled kmem
 * garbage), stable across mount/unmount cycles, and collision-free by
 * construction: real block majors live below bdevcnt (32) and getudev never
 * hands the same major out twice.
 *
 * proc and fd are single-instance and use minor 0; cdfs is per-drive, so the
 * 18 minor bits (L_MAXMIN) carry the (card,unit) pair and distinct drives get
 * distinct devs under the one latched major.
 *
 * Fallback: getudev() returns -1 only when majors up to 127 are exhausted
 * (~96 spare on this kernel — cannot happen). prinit's fallback is major 0
 * with a CE_WARN; ours is L_MAXMAJ (255), which no bdevsw slot (major < 32)
 * and no getudev consumer (major <= 127) can ever alias — the exact scheme
 * this tree already proved on metal, kept as the belt-and-suspenders default
 * in cdfs_makedev() for the unreachable case that a mount precedes cdfsinit()
 * (it cannot: mount(2) dispatches through the vsw_vfsops pointer that only
 * cdfsinit() installs).
 */
#define CDFS_CARD_MASK  0x3ffU          /* 10 minor bits for the card ... */
#define CDFS_UNIT_MASK  0xffU           /* ... and 8 for the unit = 18 = L_MAXMIN */

/* makedevice(major, 0) — latched once by cdfsinit(), see above. The module
 * form's cdfsinit and this variable live in cdfs_modlocal.c. */
#if !defined(CDFS_MODULE)
static dev_t cdfs_devbase = (dev_t)0;
#endif

/*
 * The duplicate-drive guard's other half: the module form claims the drive in
 * the guard (it has no vfs_devsearch, see cdfs_mod.h) and gives it back on
 * every failed mount and at unmount. The static object has nothing to give
 * back: vfs_devsearch() reads the live vfs list.
 */
#if defined(CDFS_MODULE)
#define CDFS_DRIVE_RELEASE(dev) cdfs_drive_release(dev)
#else
#define CDFS_DRIVE_RELEASE(dev) ((void)0)
#endif

static dev_t cdfs_makedev(unsigned int card, unsigned int unit)
{
    unsigned int m;

    m = ((card & CDFS_CARD_MASK) << 8) | (unit & CDFS_UNIT_MASK);
    if (cdfs_devbase == (dev_t)0)
        return (dev_t)makedevice(L_MAXMAJ, m);   /* unreachable; see above */
    return cdfs_devbase | (dev_t)m;
}

/* ---- vfs ops (positional, matching <sys/vfs.h> struct vfsops) -------- */

/*
 * Block-cache size per mount. The core default (ODFS_BLOCK_CACHE_SIZE, 128)
 * makes the slab one 264 204-byte kmem request = 65 contiguous 4 KiB sptmap
 * pages, which a fragmented sptmap cannot always supply (stock kmem then
 * sleeps forever on Km_OsizeWanted). 32 blocks = 67 596 bytes = 17 pages and
 * keeps read-ahead (it needs >= 32). File reads bypass the block cache for
 * whole aligned sectors, so the smaller cache costs little. If even the slab
 * fails, odfs_cache_init falls back to one aligned buffer per entry (32 x 2
 * pages, non-contiguous). See docs/amix-kmem-oversize.md.
 */
#define CDFS_CACHE_BLOCKS 32U

/* 1: vfs_mount(vfsp, mvp, uap, cr) */
static int cdfs_mount(struct vfs *vfsp, struct vnode *mvp,
                      struct mounta *uap, cred_t *cr)
{
    char             specbuf[64];
    size_t           done;
    unsigned int     card;
    unsigned int     unit;
    struct cdfs_vfs *cvfs;
    struct vnode    *rvp;
    odfs_media_t     media;
    odfs_mount_opts_t opts;
    odfs_err_t       oe;
    dev_t            dev;
    int              e;

    if (uap == 0 || uap->spec == 0)
        return EINVAL;

    /* Privilege: mount(2) checks none (disassembled — no suser call in it);
     * every filesystem in this kernel opens its VFS_MOUNT with suser. */
    if (!suser(cr))
        return EPERM;

    if (mvp->v_type != VDIR)
        return ENOTDIR;

    /*
     * THE ANTI-STACKING GUARD — the check that makes umount(2) trustworthy.
     *
     * mount(2)'s only own busy test is v_vfsmountedhere on the vnode
     * lookupname() returned for the mount point. But for a directory that is
     * ALREADY mounted on, lookupname TRAVERSES into the mounted filesystem
     * and returns its ROOT vnode — whose v_vfsmountedhere is clear. So
     * without this guard a second `mount -F cdfs 0,2 /cdrom` does not EBUSY:
     * it silently mounts ON TOP of the first mount (rc=0), and umount(2) —
     * which resolves its path the same traversing way — then pops ONE layer
     * per call, returns 0, and `ls /cdrom` still shows the disc beneath:
     * "umount succeeded but the CD is still mounted".
     *
     * Every other filesystem in this kernel refuses exactly this, verified by
     * disassembly: prmount and fdmount reject v_count > 1, VROOT, and an
     * already-mounted instance; s5mount rejects v_count > 1 and VROOT on a
     * non-remount. A covered directory can never carry VROOT; a traversed-to
     * mount point always does. The v_count test is meaningful here because
     * mount(2) has already dnlc_purge_vp()'d the mount point before
     * dispatching VFS_MOUNT, so the only legitimate hold left is
     * lookupname's own.
     *
     * No remount carve-out is needed (s5mount skips these checks when
     * remounting): a remount reaches VFS_MOUNT with mvp = the mounted fs's
     * ROOT vnode, so the VROOT test below refuses it — correctly, since a
     * fixed read-only filesystem has nothing a remount could change. And
     * mount(2)'s MS_REMOUNT path requires MS_RDONLY to be CLEAR (disassembled:
     * remount + MS_RDONLY returns EINVAL up front), so any remount that
     * somehow passed here would still be refused by the EROFS gate below.
     */
    if (mvp->v_count > 1 || (mvp->v_flag & VROOT))
        return EBUSY;

    /*
     * cdfs is read-only, and ONLY MS_RDONLY can keep it that way. vfs_add()
     * runs AFTER this routine returns and rewrites VFS_RDONLY from the mount(2)
     * flags alone:
     *
     *     if (mflag & MS_RDONLY) vfs_flag |=  VFS_RDONLY;
     *     else                   vfs_flag &= ~VFS_RDONLY;
     *
     * so the VFS_RDONLY we set below is CLEARED again for any caller that did
     * not pass MS_RDONLY — leaving the kernel believing a CD is writable. Our
     * userland helper (platform/amix/mount.c) always passes it; refuse anybody
     * who does not, rather than mount read-only media the kernel thinks it may
     * write to. (vfs_add leaves VFS_NOTRUNC alone, so that one does survive.)
     */
    if ((uap->flags & MS_RDONLY) == 0)
        return EROFS;

    e = copyinstr(uap->spec, specbuf, sizeof(specbuf), &done);
    if (e != 0)
        return e;
    specbuf[sizeof(specbuf) - 1] = '\0';

    if (cdfs_parse_spec(specbuf, &card, &unit) != 0)
        return EINVAL;

    /*
     * s5mount's duplicate-device guard, mirrored: the same drive must not be
     * mountable twice (at a second mount point either). Possible only because
     * vfs_dev is deterministic — the pre-0fcf308 recycled-garbage dev could
     * never have supported this check.
     */
    dev = cdfs_makedev(card, unit);
#if defined(CDFS_MODULE)
    if (cdfs_drive_claim(dev) != 0)
#else
    if (vfs_devsearch(dev) != 0)
#endif
        return EBUSY;

    /* sizeof(*cvfs) is over 4096 bytes, a kmem oversize request: fail with
     * ENOMEM rather than sleep forever on Km_OsizeWanted (see odfs/alloc.h). */
    cvfs = (struct cdfs_vfs *)kmem_zalloc(sizeof(*cvfs), KM_NOSLEEP);
    if (cvfs == 0) {
        CDFS_DRIVE_RELEASE(dev);
        return ENOMEM;
    }
    cvfs->card = card;
    cvfs->unit = unit;

    oe = odfs_kern_media_open_scsi(card, unit, &media);
    if (oe != ODFS_OK) {
        kmem_free((void *)cvfs, sizeof(*cvfs));
        CDFS_DRIVE_RELEASE(dev);
        return cdfs_errno(oe);
    }

    odfs_mount_opts_default(&opts);
    opts.cache_blocks = CDFS_CACHE_BLOCKS;
    oe = odfs_mount(&media, &opts, (odfs_log_state_t *)0, &cvfs->mnt);
    if (oe != ODFS_OK) {
        odfs_media_close(&media);   /* odfs_mount does not close on failure */
        kmem_free((void *)cvfs, sizeof(*cvfs));
        CDFS_DRIVE_RELEASE(dev);
        return cdfs_errno(oe);
    }

    /*
     * Fill in the vfs the way stock s5mount does — and deliberately NOT with
     * VFS_INIT().
     *
     * mount(2) has ALREADY run VFS_INIT on this struct (it is inlined there,
     * right after the kmem_alloc), and has then called vfs_lock(), which sets
     * VFS_MLOCK in vfs_flag. VFS_INIT *assigns* `vfs_flag = 0`, so calling it a
     * second time here destroys that lock bit. Worse, on an MS_REMOUNT mount(2)
     * hands us the LIVE vfs rather than a fresh one, and VFS_INIT would then
     * also zero vfs_next — truncating the global rootvfs list.
     *
     * s5mount calls VFS_INIT nowhere. It never writes vfs_op, vfs_next or
     * vfs_nsubmounts (mount(2) owns those), it only ever ORs vfs_flag, and it
     * sets exactly the fields below. Do the same. (`dev` was synthesized up at
     * the duplicate-device guard.)
     */

    /* Already set by mount(2) from the vfssw row cdfsinit() filled in — it had
     * to be, or we could not have been dispatched through it. Idempotent, and
     * kept explicit so the vector this vfs runs on is stated at the mount. */
    vfsp->vfs_op     = &cdfs_vfsops;

    /* Before cdfs_getnode(): it reaches cvfs back through VFSTOCDFS(vfsp). */
    vfsp->vfs_data   = (caddr_t)cvfs;

    vfsp->vfs_fstype = cdfs_fstype;
    vfsp->vfs_bsize  = 2048;

    /* The three mount(2)'s kmem_alloc leaves as recycled garbage. s5mount sets
     * dev and fsid to (device, fstype) and zeroes the I/O count; ours is the
     * synthesized (card,unit) device — see cdfs_makedev(). */
    vfsp->vfs_dev         = dev;
    vfsp->vfs_fsid.val[0] = (long)dev;
    vfsp->vfs_fsid.val[1] = (long)cdfs_fstype;
    vfsp->vfs_bcount      = 0;

    /* OR, never assign: VFS_MLOCK is live in here right now. */
    vfsp->vfs_flag  |= VFS_RDONLY | VFS_NOTRUNC;

    e = cdfs_getnode(vfsp, &cvfs->mnt.root,
                     (const odfs_node_t *)0, (const char *)0, &rvp);
    if (e != 0) {
        odfs_unmount(&cvfs->mnt);
        kmem_free((void *)cvfs, sizeof(*cvfs));
        CDFS_DRIVE_RELEASE(dev);
        return e;
    }
    rvp->v_flag |= VROOT;
    cvfs->rootvp = rvp;   /* the mount owns this reference */

    return 0;
}

/* 2: vfs_unmount(vfsp, cr) */
static int cdfs_unmount(struct vfs *vfsp, cred_t *cr)
{
    struct cdfs_vfs *cvfs = VFSTOCDFS(vfsp);
    struct cdfsnode *cnp;
    unsigned int     h;

    /* Busy check: the only live vnode may be the root, held once by the mount.
     * Any other node in the hash, or an extra root hold, means EBUSY. */
    for (h = 0; h < CDFS_NHASH; h++) {
        for (cnp = cvfs->hash[h]; cnp != 0; cnp = cnp->hash_next) {
            struct vnode *vp = CDFSTOV(cnp);
            if (vp == cvfs->rootvp) {
                if (vp->v_count > 1)
                    return EBUSY;
            } else {
                return EBUSY;
            }
        }
    }

    /* Drop the mount's root hold; v_count hits 0, vn_rele calls vop_inactive,
     * which unlinks + frees the root node. Do this BEFORE freeing cvfs, since
     * inactive reaches cvfs through vp->v_vfsp. */
    if (cvfs->rootvp != 0)
        VN_RELE(cvfs->rootvp);

    odfs_unmount(&cvfs->mnt);

    if (cvfs->devvp != 0)
        VN_RELE(cvfs->devvp);

    kmem_free((void *)cvfs, sizeof(*cvfs));

    CDFS_DRIVE_RELEASE(vfsp->vfs_dev);
    return 0;
}

/* 3: vfs_root(vfsp, vpp) */
static int cdfs_root(struct vfs *vfsp, struct vnode **vpp)
{
    struct cdfs_vfs *cvfs = VFSTOCDFS(vfsp);
    struct vnode    *vp = cvfs->rootvp;

    if (vp == 0)
        return EIO;
    VN_HOLD(vp);
    *vpp = vp;
    return 0;
}

/* 4: vfs_statvfs(vfsp, sp) */
static int cdfs_statvfs(struct vfs *vfsp, struct statvfs *sp)
{
    struct cdfs_vfs *cvfs = VFSTOCDFS(vfsp);
    odfs_mount_t    *mnt = &cvfs->mnt;

    (void)memset((void *)sp, 0, sizeof(*sp));

    sp->f_bsize  = 2048;
    sp->f_frsize = 2048;
    sp->f_blocks = (u_long)mnt->total_blocks;   /* 2048-byte logical blocks */
    sp->f_bfree  = 0;
    sp->f_bavail = 0;

    /* No true inode count on an optical volume; total_blocks is a safe upper
     * bound, and the medium is full (no free nodes). */
    sp->f_files  = (u_long)mnt->total_blocks;
    sp->f_ffree  = 0;
    sp->f_favail = 0;

    sp->f_fsid = vfsp->vfs_dev;
    (void)strncpy(sp->f_basetype, "cdfs", sizeof(sp->f_basetype) - 1);
    sp->f_flag = ST_RDONLY | ST_NOTRUNC;
    sp->f_namemax = 255;
    (void)strncpy(sp->f_fstr, mnt->volume_name, sizeof(sp->f_fstr) - 1);

    return 0;
}

/* 5: vfs_sync(vfsp, flag, cr) — read-only: nothing to flush. */
static int cdfs_sync(struct vfs *vfsp, int flag, cred_t *cr)
{
    return 0;
}

/* 6: vfs_vget(vfsp, vpp, fidp) — resolve a fid whose payload is extent.lba.
 * v1 resolves only currently-cached nodes; stateless handle synthesis for NFS
 * export is out of scope. */
static int cdfs_vget(struct vfs *vfsp, struct vnode **vpp, struct fid *fidp)
{
    struct cdfs_vfs *cvfs = VFSTOCDFS(vfsp);
    struct cdfs_fid *cfid = (struct cdfs_fid *)fidp;
    struct cdfsnode *cnp;
    uint32_t         lba = cfid->cfid_lba;
    unsigned int     h = (unsigned int)odfs_umod32(lba, CDFS_NHASH);

    for (cnp = cvfs->hash[h]; cnp != 0; cnp = cnp->hash_next) {
        if (cnp->node.extent.lba == lba) {
            struct vnode *vp = CDFSTOV(cnp);
            VN_HOLD(vp);
            *vpp = vp;
            return 0;
        }
    }

    *vpp = 0;
    return EINVAL;
}

/* 7: vfs_mountroot(vfsp, why) — cdfs can never be the root filesystem. */
static int cdfs_mountroot(struct vfs *vfsp, enum whymountroot why)
{
    return EROFS;
}

/* 8: vfs_swapvp(vfsp, vpp, nm) — no swap on a read-only optical volume. */
static int cdfs_swapvp(struct vfs *vfsp, struct vnode **vpp, char *nm)
{
    return EINVAL;
}

/* ---- vfssw init routine --------------------------------------------- */

/*
 * cdfsinit() — the vsw_init routine, called once at boot as the kernel walks
 * vfssw[] initializing each filesystem type. ROBUST to the kernel's vsw_init
 * calling convention: rather than trusting a passed (vswp, fstype) — the AMIX
 * boot loop invokes (*vsw_init)() with no/other arguments, so a declared vswp
 * would be garbage and writing through it corrupts an unrelated slot, leaving
 * the real "cdfs" row's vsw_vfsops NULL (mount(2) then EINVALs before dispatch)
 * — it SELF-LOCATES its own row in the kernel's global vfssw[] table by name,
 * installs the op vector there, and records the row index as the fstype.
 *
 * Declared with empty parens (C89 "unspecified arguments") so it binds cleanly
 * however the kernel invokes it; any arguments the kernel pushes are ignored.
 *
 * The vfssw[] row it locates,
 *   { "cdfs", cdfsinit, 0, 0 }
 * is added by the kernel-build step (out of scope). vfssw[]/nfstype come from
 * <sys/vfs.h>; strcmp is the kernel's own copy (declared in <string.h>). All
 * three stay undefined externs in libcdfskern.o, resolved at the kernel link.
 */
#if !defined(CDFS_MODULE)   /* the module form's cdfsinit: cdfs_modlocal.c */
int cdfsinit()
{
    int i;
    int udev;

    /*
     * Latch the synthesized device major, the way prinit()/fdinit() do (they
     * run in this same boot-time vfssw init walk): one getudev() major for the
     * life of the boot, minor bits carry (card,unit) per mount. Fallback to
     * L_MAXMAJ on exhaustion — see the synthesized-device-identity block.
     */
    if (cdfs_devbase == (dev_t)0) {
        udev = getudev();
        if (udev == -1)
            udev = (int)L_MAXMAJ;
        cdfs_devbase = (dev_t)makedevice((unsigned int)udev, 0);
    }

    for (i = 0; i < nfstype; i++) {
        if (vfssw[i].vsw_name != 0 &&
            strcmp(vfssw[i].vsw_name, "cdfs") == 0) {
            vfssw[i].vsw_vfsops = &cdfs_vfsops;
            cdfs_fstype = i;
            return 0;
        }
    }
    return 0;
}
#endif /* !CDFS_MODULE */

/* ---- the vector: positional, EXACT <sys/vfs.h> struct vfsops order --- */

struct vfsops cdfs_vfsops = {
    cdfs_mount,      /* 1 vfs_mount     */
    cdfs_unmount,    /* 2 vfs_unmount   */
    cdfs_root,       /* 3 vfs_root      */
    cdfs_statvfs,    /* 4 vfs_statvfs   */
    cdfs_sync,       /* 5 vfs_sync      */
    cdfs_vget,       /* 6 vfs_vget      */
    cdfs_mountroot,  /* 7 vfs_mountroot */
    cdfs_swapvp      /* 8 vfs_swapvp    */
    /* vfs_filler[8] left zero-initialized */
};

#endif /* ODFS_PLATFORM_AMIX_KERNEL */
