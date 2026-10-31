/*
 * cdfs_vnodeops.c — the SVR4 vnodeops vector for the AMIX in-kernel cdfs.
 *
 * Defines cdfs_vnodeops POSITIONALLY, in the EXACT 37-slot order of struct
 * vnodeops in <sys/vnode.h>. The vector is a K&R `int (*)()` array (with three
 * void slots: vop_inactive, vop_rwlock, vop_rwunlock) and no compile-time
 * checking, so the VOP_* call macros in <sys/vnode.h> are the ABI; every
 * function's argument list matches its macro exactly. Slots this read-only fs
 * does not implement point at the kernel's shared fs_subr stubs (fs_nosys /
 * fs_setfl / fs_frlock / fs_poll / fs_pathconf / fs_cmp / fs_rwlock /
 * fs_rwunlock); every mutating op funnels to cdfs_rdonly() (EROFS). No slot is
 * NULL — the generic vnode layer dereferences each unconditionally.
 *
 * C89-clean for the period cross compiler (GCC 2.7.2.3).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#if defined(ODFS_PLATFORM_AMIX_KERNEL)

#include "cdfs.h"
#include "cdfs_blocks.h"    /* cdfs_bytes_to_dev_blocks() — st_blocks math */

#include <sys/errno.h>
#include <sys/uio.h>        /* struct uio, uiomove, UIO_READ */
#include <sys/dirent.h>     /* struct dirent */
#include <sys/cred.h>       /* cred_t */
#include <sys/file.h>       /* FWRITE */
#include <sys/pathname.h>   /* struct pathname (vop_lookup) */
#include <sys/kmem.h>       /* kmem_alloc/kmem_free, KM_NOSLEEP */
#include <fs/fs_subr.h>     /* fs_nosys/fs_setfl/fs_frlock/fs_poll/... */
#include "odfs/ancestry.h"  /* odfs_resolve_parent_node() */
#include "odfs/alloc.h"     /* odfs_calloc/odfs_free (node scratch off the stack) */
#include <string.h>         /* strlen, memset, memcpy */

/*
 * On-stack kernel space is scarce; read/dir/link scratch comes from kmem.
 *
 * The read staging buffer is 4096 bytes, the largest request the stock kmem
 * serves from its big buddy pool: a larger one is "oversize", whole
 * kernel-map pages that a fragmented map may not have. The media sends one
 * sector per SCSI command (AMIX_KERN_MAX_XFER), so a smaller buffer adds no
 * commands. DMA safety does not depend on its alignment: the media bounces
 * any target that is not 2048-aligned and refuses a transfer that crosses a
 * page. A big-pool block is size-aligned within 4096-aligned pool regions,
 * so whole sectors still go straight into it.
 */
#define CDFS_RDBUF 4096

/* ---- simple ops ----------------------------------------------------- */

/* 1: vop_open(vpp, flag, cr) */
static int cdfs_open(struct vnode **vpp, int flag, cred_t *cr)
{
    if (flag & FWRITE)
        return EROFS;
    return 0;
}

/* 2: vop_close(vp, flag, count, offset, cr) */
static int cdfs_close(struct vnode *vp, int flag, int count,
                      off_t offset, cred_t *cr)
{
    return 0;
}

/* Shared mutator stub: every write-path op returns EROFS. K&R (no prototype)
 * so one body serves slots with differing argument lists. */
static int cdfs_rdonly()
{
    return EROFS;
}

/* 3: vop_read(vp, uiop, ioflag, cr) */
static int cdfs_read(struct vnode *vp, struct uio *uiop, int ioflag, cred_t *cr)
{
    struct cdfsnode *cnp = VTOCDFS(vp);
    odfs_mount_t    *mnt = &VFSTOCDFS(vp->v_vfsp)->mnt;
    char            *buf;
    int              e = 0;

    if (vp->v_type == VDIR)
        return EISDIR;
    if (uiop->uio_offset < 0)
        return EINVAL;

    buf = (char *)kmem_alloc((size_t)CDFS_RDBUF, KM_NOSLEEP);
    if (buf == 0)
        return ENOMEM;

    while (uiop->uio_resid > 0) {
        size_t     len = (size_t)CDFS_RDBUF;
        odfs_err_t oe;

        if ((int)len > uiop->uio_resid)
            len = (size_t)uiop->uio_resid;

        oe = odfs_read(mnt, &cnp->node, (uint64_t)uiop->uio_offset, buf, &len);
        if (oe != ODFS_OK && oe != ODFS_ERR_EOF) {
            e = cdfs_errno(oe);
            break;
        }
        if (len == 0)
            break;              /* end of file */

        e = uiomove((caddr_t)buf, (int)len, UIO_READ, uiop);
        if (e != 0)
            break;
    }

    kmem_free((void *)buf, (size_t)CDFS_RDBUF);
    return e;
}

/* 7: vop_getattr(vp, vap, flags, cr) */
static int cdfs_getattr(struct vnode *vp, struct vattr *vap, int flags,
                        cred_t *cr)
{
    struct cdfsnode *cnp = VTOCDFS(vp);
    odfs_node_t     *n = &cnp->node;
    u_long           perms;

    vap->va_type = vp->v_type;

    if (n->mode != 0)
        perms = (u_long)(n->mode & 07777);
    else
        perms = (n->kind == ODFS_NODE_DIR) ? 0555u : 0444u;
    vap->va_mode = (mode_t)perms;

    vap->va_uid = 0;
    vap->va_gid = 0;
    vap->va_fsid = vp->v_vfsp->vfs_dev;
    vap->va_nodeid = (ino_t)n->extent.lba;
    vap->va_nlink = (nlink_t)((n->kind == ODFS_NODE_DIR) ? 2 : 1);
    vap->va_size0 = 0;
    vap->va_size = (u_long)n->size;
    cdfs_time_to_timestruc(&n->mtime, &vap->va_mtime);
    vap->va_atime = vap->va_mtime;
    cdfs_time_to_timestruc(&n->ctime, &vap->va_ctime);
    vap->va_rdev = 0;
    vap->va_blksize = 2048;
    /* st_blocks is in DEV_BSIZE (512 B) units, rounded up to a whole 2048-byte
     * logical sector — see cdfs_blocks.h for the SVR4 authority and for why
     * this must NOT be written as a 64-bit divide (on metal the resulting
     * __udivdi3 call returns a normalization intermediate rather than the
     * quotient; the mechanism is still unidentified). n->size is truncated
     * exactly as va_size is, since the SVR4 vattr is 32-bit throughout. */
    vap->va_nblocks = (u_long)cdfs_bytes_to_dev_blocks((unsigned long)n->size);
    vap->va_vcode = 0;

    return 0;
}

/* 9: vop_access(vp, mode, flags, cr) */
static int cdfs_access(struct vnode *vp, int mode, int flags, cred_t *cr)
{
    if (mode & VWRITE)
        return EROFS;
    return 0;
}

/* 10: vop_lookup(dvp, nm, vpp, pnp, flags, rdir, cr) */
static int cdfs_lookup(struct vnode *dvp, char *nm, struct vnode **vpp,
                       struct pathname *pnp, int flags, struct vnode *rdir,
                       cred_t *cr)
{
    struct cdfsnode *cnp = VTOCDFS(dvp);
    odfs_mount_t    *mnt = &VFSTOCDFS(dvp->v_vfsp)->mnt;
    odfs_node_t     *scratch;   /* the child (name lookup) or the parent ("..").
                                 * Heap, not a ~660 B stack local (kernel
                                 * ODFS_NAME_MAX=512): two of these on the tiny
                                 * SVR4 kernel stack, nested under the core walk,
                                 * would overrun it. Allocated lazily so the "."
                                 * / VROOT ".." fast paths take no allocation. */
    odfs_err_t       oe;
    int              e;

    if (dvp->v_type != VDIR)
        return ENOTDIR;

    /* "." (and the empty component) resolve to the directory itself. */
    if (nm[0] == '\0' || (nm[0] == '.' && nm[1] == '\0')) {
        VN_HOLD(dvp);
        *vpp = dvp;
        return 0;
    }

    /* ".." — root is its own parent; otherwise walk the tree. */
    if (nm[0] == '.' && nm[1] == '.' && nm[2] == '\0') {
        if (dvp->v_flag & VROOT) {
            VN_HOLD(dvp);
            *vpp = dvp;
            return 0;
        }
        scratch = odfs_calloc(1, sizeof(*scratch));
        if (scratch == 0)
            return ENOMEM;
        oe = odfs_resolve_parent_node(mnt, &cnp->node, scratch,
                                      (odfs_node_t *)0);
        if (oe != ODFS_OK) {
            odfs_free(scratch);
            return cdfs_errno(oe);
        }
        e = cdfs_getnode(dvp->v_vfsp, scratch,
                         (const odfs_node_t *)0, (const char *)0, vpp);
        odfs_free(scratch);
        return e;
    }

    scratch = odfs_calloc(1, sizeof(*scratch));
    if (scratch == 0)
        return ENOMEM;

    oe = odfs_lookup(mnt, &cnp->node, nm, scratch);
    if (oe != ODFS_OK) {
        odfs_free(scratch);
        return cdfs_errno(oe);
    }

    /* Pass the parent + name so a symlink child can service readlink. */
    e = cdfs_getnode(dvp->v_vfsp, scratch, &cnp->node, nm, vpp);
    odfs_free(scratch);
    return e;
}

/* ---- readdir -------------------------------------------------------- */

typedef struct cdfs_rd_ctx {
    struct uio *uiop;
    char       *scratch;    /* dirent build buffer */
    size_t      scratchsz;  /* its size, so emit can bound its writes */
    uint32_t    skip;       /* real children already delivered (to skip) */
    uint32_t    seen;       /* real children visited this call */
    uint32_t    emitted;    /* real children written to the uio this call */
    int         full;       /* uio ran out of room */
    int         err;        /* uiomove error (errno) */
} cdfs_rd_ctx_t;

/*
 * Format one directory entry into the caller's uio. Returns 0 on success,
 * -1 if the record does not fit (caller stops), or a positive errno on a
 * uiomove fault. d_reclen is the fixed header (offsetof d_name) plus the name
 * and its NUL, rounded up to 4 bytes (the ufs DIRSIZ convention; there is no
 * reclen macro in <sys/dirent.h>).
 *
 * The name length is established with a BOUNDED scan, never strlen(): `name`
 * is odfs_node_t.name, a char[ODFS_NAME_MAX], and the memset/memcpy below
 * write into `scratch`, which is sized hdr + ODFS_NAME_MAX + slack — while the
 * pre-existing reclen check only bounded against uio_resid (~8 KiB). If a
 * backend ever handed back a name with no NUL in the array, strlen would walk
 * off the end of the node struct, reclen would sail past the uio_resid check,
 * and the memset would scribble kilobytes across the kernel heap. So: scan at
 * most ODFS_NAME_MAX bytes, and clamp an unterminated or oversized name to
 * what scratch can hold (silently — the entry is still delivered, truncated,
 * rather than corrupting memory or aborting the whole readdir).
 */
static int cdfs_emit_dirent(struct uio *uiop, char *scratch, size_t scratchsz,
                            ino_t d_ino, off_t d_off, const char *name)
{
    struct dirent *dp = (struct dirent *)scratch;
    size_t         hdr = offsetof(struct dirent, d_name);
    size_t         namelen;
    size_t         reclen;
    size_t         need;
    size_t         cap;

    /* Bounded scan: never read past the end of odfs_node_t.name[]. */
    for (namelen = 0; namelen < (size_t)ODFS_NAME_MAX; namelen++) {
        if (name[namelen] == '\0')
            break;
    }

    need   = hdr + namelen + 1u;          /* bytes this must put in scratch */
    reclen = (need + 3u) & ~((size_t)3);

    if (reclen > scratchsz) {
        /* Clamp the name so every write below stays inside `scratch`. */
        cap = scratchsz & ~((size_t)3);   /* largest 4-aligned reclen that fits */
        if (cap < hdr + 4u)
            return -1;                    /* scratch cannot hold even an empty name */
        if (namelen > cap - hdr - 1u)
            namelen = cap - hdr - 1u;

        need   = hdr + namelen + 1u;
        reclen = (need + 3u) & ~((size_t)3);
    }

    if ((int)reclen > uiop->uio_resid)
        return -1;

    dp->d_ino = d_ino;
    dp->d_off = d_off;
    dp->d_reclen = (unsigned short)reclen;
    (void)memset(dp->d_name, 0, reclen - hdr);
    (void)memcpy(dp->d_name, name, namelen);

    return uiomove((caddr_t)dp, (int)reclen, UIO_READ, uiop);
}

static odfs_err_t cdfs_readdir_cb(const odfs_node_t *entry, void *ctx)
{
    cdfs_rd_ctx_t *c = (cdfs_rd_ctx_t *)ctx;
    off_t          d_off;
    int            r;

    if (c->seen < c->skip) {
        c->seen++;                 /* already delivered in a prior call */
        return ODFS_OK;
    }

    /* Cookie to resume AFTER this child: 2 synthesized entries + (seen+1). */
    d_off = (off_t)(2u + c->seen + 1u);
    r = cdfs_emit_dirent(c->uiop, c->scratch, c->scratchsz,
                         (ino_t)entry->extent.lba, d_off, entry->name);
    if (r == -1) {
        c->full = 1;
        return ODFS_ERR_EOF;       /* stop iterating: no room */
    }
    if (r != 0) {
        c->err = r;
        return ODFS_ERR_EOF;       /* stop iterating: fault */
    }

    c->seen++;
    c->emitted++;
    return ODFS_OK;
}

/*
 * 17: vop_readdir(vp, uiop, cr, eofp).
 *
 * The cookie in uio_offset is an ENTRY INDEX: 0 = ".", 1 = "..", 2+ = children.
 * (uiomove advances uio_offset by bytes moved as a side effect; this op
 * overwrites uio_offset with the entry-index cursor at the end.) The backend
 * exposes its resume offset only when the visitor callback stops, and SVR4
 * vop_readdir carries no per-open state beyond uio_offset, so children are
 * re-walked from the start each call and the first `skip` are dropped — O(n*k)
 * across a directory, acceptable for a v1 read-only optical fs (the block +
 * parsed-dir caches absorb the re-walk; CD directories are small).
 */
static int cdfs_readdir(struct vnode *vp, struct uio *uiop, cred_t *cr,
                        int *eofp)
{
    struct cdfsnode *cnp = VTOCDFS(vp);
    odfs_mount_t    *mnt = &VFSTOCDFS(vp->v_vfsp)->mnt;
    cdfs_rd_ctx_t    c;
    char            *scratch;
    size_t           scratchsz;
    uint32_t         startidx;
    uint32_t         i;
    uint32_t         synth_emitted = 0;
    int              ran_phase2 = 0;
    int              hit_eof = 0;
    int              e = 0;

    if (vp->v_type != VDIR)
        return ENOTDIR;
    if (uiop->uio_offset < 0)
        return EINVAL;

    scratchsz = offsetof(struct dirent, d_name) + (size_t)ODFS_NAME_MAX + 8u;
    scratch = (char *)kmem_alloc(scratchsz, KM_NOSLEEP);
    if (scratch == 0)
        return ENOMEM;

    startidx = (uint32_t)uiop->uio_offset;

    c.uiop = uiop;
    c.scratch = scratch;
    c.scratchsz = scratchsz;
    c.skip = 0;
    c.seen = 0;
    c.emitted = 0;
    c.full = 0;
    c.err = 0;

    /* Phase 1: synthesize "." (index 0) and ".." (index 1). Both carry this
     * directory's own nodeid for d_ino — a v1 simplification; ".." traversal
     * uses vop_lookup, not this d_ino. ISO/RR/Joliet omit dot entries from
     * odfs_readdir, so there is no duplication with phase 2. */
    for (i = startidx; i < 2u; i++) {
        const char *dn = (i == 0u) ? "." : "..";
        int         r;

        r = cdfs_emit_dirent(uiop, scratch, scratchsz,
                             (ino_t)cnp->node.extent.lba,
                             (off_t)(i + 1u), dn);
        if (r == -1) {
            c.full = 1;
            break;
        }
        if (r != 0) {
            c.err = r;
            break;
        }
        synth_emitted++;
    }

    /* Phase 2: real children (skipping those already delivered). */
    if (!c.full && c.err == 0) {
        odfs_err_t oe;

        ran_phase2 = 1;
        c.skip = (startidx >= 2u) ? (startidx - 2u) : 0u;
        oe = odfs_readdir(mnt, &cnp->node, cdfs_readdir_cb, &c,
                          (uint32_t *)0);
        if (c.err != 0)
            e = c.err;
        else if (c.full)
            hit_eof = 0;                 /* stopped for space */
        else if (oe == ODFS_OK || oe == ODFS_ERR_EOF)
            hit_eof = 1;                 /* directory exhausted */
        else
            e = cdfs_errno(oe);          /* genuine backend error */
    } else if (c.err != 0) {
        e = c.err;
    }

    if (e == 0) {
        if (ran_phase2)
            uiop->uio_offset = (off_t)(2u + c.seen);
        else
            uiop->uio_offset = (off_t)(startidx + synth_emitted);
        if (eofp != 0)
            *eofp = hit_eof;
    } else if (eofp != 0) {
        *eofp = 0;
    }

    kmem_free((void *)scratch, scratchsz);
    return e;
}

/* 19: vop_readlink(vp, uiop, cr) */
static int cdfs_readlink(struct vnode *vp, struct uio *uiop, cred_t *cr)
{
    struct cdfsnode *cnp = VTOCDFS(vp);
    odfs_mount_t    *mnt = &VFSTOCDFS(vp->v_vfsp)->mnt;
    char            *buf;
    odfs_err_t       oe;
    size_t           len;
    int              e;

    if (vp->v_type != VLNK || cnp->slink == 0)
        return EINVAL;

    buf = (char *)kmem_alloc((size_t)ODFS_NAME_MAX, KM_NOSLEEP);
    if (buf == 0)
        return ENOMEM;

    oe = odfs_readlink(mnt, &cnp->slink->parent, cnp->slink->name,
                       buf, (size_t)ODFS_NAME_MAX);
    if (oe != ODFS_OK) {
        kmem_free((void *)buf, (size_t)ODFS_NAME_MAX);
        return cdfs_errno(oe);
    }

    /* Bounded, for the same reason cdfs_emit_dirent never strlen()s: a
     * target that filled buf with no NUL would send strlen off the end. */
    for (len = 0; len < (size_t)ODFS_NAME_MAX; len++) {
        if (buf[len] == '\0')
            break;
    }
    if ((int)len > uiop->uio_resid)
        len = (size_t)uiop->uio_resid;
    e = uiomove((caddr_t)buf, (int)len, UIO_READ, uiop);

    kmem_free((void *)buf, (size_t)ODFS_NAME_MAX);
    return e;
}

/* 20: vop_fsync(vp, cr) — read-only: nothing to flush. */
static int cdfs_fsync(struct vnode *vp, cred_t *cr)
{
    return 0;
}

/* 21: vop_inactive(vp, cr) — VOID. Last reference gone: free the node. */
static void cdfs_inactive(struct vnode *vp, cred_t *cr)
{
    cdfs_node_inactive(vp);
}

/* 22: vop_fid(vp, fidp) — emit a handle whose payload is extent.lba. */
static int cdfs_fid(struct vnode *vp, struct fid *fidp)
{
    struct cdfsnode *cnp = VTOCDFS(vp);
    struct cdfs_fid *cfid = (struct cdfs_fid *)fidp;

    cfid->cfid_len = (u_short)(sizeof(struct cdfs_fid) - sizeof(u_short));
    cfid->cfid_lba = cnp->node.extent.lba;
    return 0;
}

/* 25: vop_seek(vp, ooff, noffp) — reject a negative resulting offset. */
static int cdfs_seek(struct vnode *vp, off_t ooff, off_t *noffp)
{
    if (*noffp < 0)
        return EINVAL;
    return 0;
}

/* 29: vop_realvp(vp, vpp) — cdfs vnodes are themselves the real vnode. */
static int cdfs_realvp(struct vnode *vp, struct vnode **vpp)
{
    *vpp = vp;
    return 0;
}

/* ---- the vector: positional, EXACT <sys/vnode.h> struct vnodeops order --- */

struct vnodeops cdfs_vnodeops = {
    cdfs_open,      /*  1 vop_open      */
    cdfs_close,     /*  2 vop_close     */
    cdfs_read,      /*  3 vop_read      */
    cdfs_rdonly,    /*  4 vop_write     */
    fs_nosys,       /*  5 vop_ioctl     */
    fs_setfl,       /*  6 vop_setfl     */
    cdfs_getattr,   /*  7 vop_getattr   */
    cdfs_rdonly,    /*  8 vop_setattr   */
    cdfs_access,    /*  9 vop_access    */
    cdfs_lookup,    /* 10 vop_lookup    */
    cdfs_rdonly,    /* 11 vop_create    */
    cdfs_rdonly,    /* 12 vop_remove    */
    cdfs_rdonly,    /* 13 vop_link      */
    cdfs_rdonly,    /* 14 vop_rename    */
    cdfs_rdonly,    /* 15 vop_mkdir     */
    cdfs_rdonly,    /* 16 vop_rmdir     */
    cdfs_readdir,   /* 17 vop_readdir   */
    cdfs_rdonly,    /* 18 vop_symlink   */
    cdfs_readlink,  /* 19 vop_readlink  */
    cdfs_fsync,     /* 20 vop_fsync     */
    cdfs_inactive,  /* 21 vop_inactive (void) */
    cdfs_fid,       /* 22 vop_fid       */
    fs_rwlock,      /* 23 vop_rwlock (void) */
    fs_rwunlock,    /* 24 vop_rwunlock (void) */
    cdfs_seek,      /* 25 vop_seek      */
    fs_cmp,         /* 26 vop_cmp       */
    fs_frlock,      /* 27 vop_frlock    */
    cdfs_rdonly,    /* 28 vop_space     */
    cdfs_realvp,    /* 29 vop_realvp    */
    fs_nosys,       /* 30 vop_getpage   */
    fs_nosys,       /* 31 vop_putpage   */
    fs_nosys,       /* 32 vop_map       */
    fs_nosys,       /* 33 vop_addmap    */
    fs_nosys,       /* 34 vop_delmap    */
    fs_poll,        /* 35 vop_poll      */
    fs_nosys,       /* 36 vop_dump      */
    fs_pathconf     /* 37 vop_pathconf  */
    /* vop_filler[32] left zero-initialized */
};

#endif /* ODFS_PLATFORM_AMIX_KERNEL */
