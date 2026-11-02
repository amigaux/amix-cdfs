/*
 * cdfs_node.c — vnode object cache for the AMIX in-kernel cdfs.
 *
 * The portable ODFS core returns VALUE-type odfs_node_t with no lifecycle; cdfs
 * owns vnode identity. Each mount keeps a hash of live cdfsnode objects keyed by
 * extent.lba (the disc-authoritative inode analog). cdfs_getnode() finds an
 * existing live vnode and VN_HOLDs it, or allocates and initializes a fresh one;
 * cdfs_node_inactive() (driven from vop_inactive when v_count reaches 0) unlinks
 * and frees it. This mirrors the SVR4 s5 in-core inode idiom, minus the
 * reuse-freelist: a read-only optical volume never dirties a node.
 *
 * v1 is single-threaded with respect to this hash (no bucket locking); SMP
 * hardening is a later step.
 *
 * C89-clean for the period cross compiler (GCC 2.7.2.3).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#if defined(ODFS_PLATFORM_AMIX_KERNEL)

#include "cdfs.h"
#include "odfs/arith.h"

#include <sys/kmem.h>    /* kmem_alloc/kmem_zalloc/kmem_free, KM_NOSLEEP */
#include <sys/errno.h>   /* ENOMEM, EIO, ... */
#include <string.h>      /* strncpy (kernel), + the compiler's aggregate ops */

/* Bucket for a node identity. extent.lba is the on-disc block address. */
static unsigned int cdfs_hash(uint32_t lba)
{
    return (unsigned int)odfs_umod32(lba, CDFS_NHASH);
}

vtype_t cdfs_vtype(odfs_node_kind_t kind)
{
    switch (kind) {
    case ODFS_NODE_DIR:
        return VDIR;
    case ODFS_NODE_SYMLINK:
        return VLNK;
    case ODFS_NODE_FILE:
        return VREG;
    case ODFS_NODE_VIRTUAL:
    default:
        /* Virtual nodes (e.g. a CDDA track) are file-like. The in-kernel media
         * backend exposes no audio, so CDDA is not expected to activate here. */
        return VREG;
    }
}

/* Days from 1970-01-01 to the given civil date (Howard Hinnant's algorithm,
 * valid for the proleptic Gregorian calendar). */
static long cdfs_days_from_civil(long y, unsigned int m, unsigned int d)
{
    long         era;
    unsigned int yoe;
    unsigned int doy;
    unsigned int doe;

    y -= (m <= 2);
    era = odfs_sdiv32(y >= 0 ? y : y - 399, 400);
    yoe = (unsigned int)(y - era * 400);
    doy = odfs_udiv32(153u * (m + (m > 2 ? -3u : 9u)) + 2u, 5u) + d - 1u;
    doe = yoe * 365u + yoe / 4u - odfs_udiv32(yoe, 100u) + doy;
    return era * 146097L + (long)doe - 719468L;
}

void cdfs_time_to_timestruc(const odfs_timestamp_t *ts, timestruc_t *out)
{
    long days;
    long secs;

    out->tv_nsec = 0;

    if (ts == 0 || ts->year <= 0 || ts->month < 1 || ts->month > 12) {
        out->tv_sec = 0;
        return;
    }

    days = cdfs_days_from_civil((long)ts->year,
                                (unsigned int)ts->month,
                                (unsigned int)ts->day);
    secs = days * 86400L
         + (long)ts->hour * 3600L
         + (long)ts->minute * 60L
         + (long)ts->second;

    /* Broken-down time is local; tz_offset (minutes east of UTC) maps it back
     * to the epoch. INT16_MIN marks an unknown zone — treat as UTC. */
    if (ts->tz_offset != (int16_t)0x8000)
        secs -= (long)ts->tz_offset * 60L;

    out->tv_sec = secs;
}

/*
 * odfs_err_t -> SVR4 errno.
 *
 * The distinctions this table preserves are the ONLY diagnosis a mount failure
 * gets: mount(2) returns an errno and nothing else, so every verdict collapsed
 * into EIO here is a verdict the operator cannot tell apart from a cable fault.
 * That is not hypothetical — "mount -F cdfs 1,3" against a card the kernel had
 * silently unregistered surfaced as errno 5, indistinguishable from a real read
 * error and from an unreadable disc, and was chased three times before the
 * cause was found. So EIO is reserved for "the device failed to deliver
 * readable data"; anything the code KNOWS more precisely gets its own arm.
 */
int cdfs_errno(odfs_err_t err)
{
    switch (err) {
    case ODFS_OK:
    case ODFS_ERR_EOF:
        return 0;
    case ODFS_ERR_NOMEM:
        return ENOMEM;
    case ODFS_ERR_NO_DEVICE:
        /* No controller registered at this card index — the address names
         * nothing at all. ENXIO ("no such device or address") is exactly the
         * SVR4 verdict for that, and is NOT an I/O error. */
        return ENXIO;
    case ODFS_ERR_NOT_OPTICAL:
        /* The target answered INQUIRY but is not peripheral type 0x05: the
         * device is present, just the wrong kind. ENODEV/ENXIO would both
         * read as "absent" (their strerror texts are near-identical), which
         * would re-merge this with the case above; ENOTTY is the classic UNIX
         * "inappropriate device for this operation" and is produced nowhere
         * else in cdfs, so it is an unambiguous fingerprint. */
        return ENOTTY;
    case ODFS_ERR_BAD_FORMAT:
        /* The disc read fine; no backend recognized a filesystem on it.
         * EINVAL matches how the stock SVR4 filesystems report a superblock
         * they cannot make sense of (s5mount). */
        return EINVAL;
    case ODFS_ERR_UNSUPPORTED:
        /* Recognized, but a shape this driver does not serve — a block size
         * other than 2048, a capacity past READ CAPACITY(10)'s range, or an
         * optional op with no backend behind it. Same errno as BAD_FORMAT
         * today; kept as its own arm so the two can diverge later without
         * re-deriving which call sites feed them. */
        return EINVAL;
    case ODFS_ERR_INVAL:
    case ODFS_ERR_RANGE:
    case ODFS_ERR_OVERFLOW:
        return EINVAL;
    case ODFS_ERR_NOT_FOUND:
        return ENOENT;
    case ODFS_ERR_NOT_DIR:
        return ENOTDIR;
    case ODFS_ERR_IS_DIR:
        return EISDIR;
    case ODFS_ERR_READ_ONLY:
        return EROFS;
    case ODFS_ERR_NAME_TOO_LONG:
        return ENAMETOOLONG;
    default:
        /* Genuine I/O only: IO / BAD_SECTOR / NO_MEDIA / MEDIA_CHANGED /
         * CORRUPT / LOOP. EIO out of cdfs now means the device did not
         * deliver readable data — nothing else. */
        return EIO;
    }
}

int cdfs_getnode(struct vfs *vfsp, const odfs_node_t *n,
                 const odfs_node_t *parent, const char *name,
                 struct vnode **vpp)
{
    struct cdfs_vfs *cvfs = VFSTOCDFS(vfsp);
    struct cdfsnode *cnp;
    struct vnode    *vp;
    unsigned int     h;

    h = cdfs_hash(n->extent.lba);

    /* Cache hit: reuse the live vnode. */
    for (cnp = cvfs->hash[h]; cnp != 0; cnp = cnp->hash_next) {
        if (odfs_node_matches_identity(&cnp->node, n)) {
            vp = CDFSTOV(cnp);
            VN_HOLD(vp);
            *vpp = vp;
            return 0;
        }
    }

    /* Miss: allocate + initialize a fresh node. KM_NOSLEEP, as every cdfs
     * allocation (odfs/alloc.h): under pressure the lookup fails with ENOMEM
     * rather than wait. */
    cnp = (struct cdfsnode *)kmem_zalloc(sizeof(*cnp), KM_NOSLEEP);
    if (cnp == 0)
        return ENOMEM;

    cnp->node = *n;
    cnp->slink = 0;

    /* A symlink needs its parent + entry name for odfs_readlink(). Before
     * the vnode is set up, so a failure has only cnp to give back. */
    if (n->kind == ODFS_NODE_SYMLINK && parent != 0) {
        cnp->slink = (struct cdfs_symlink *)
            kmem_zalloc(sizeof(struct cdfs_symlink), KM_NOSLEEP);
        if (cnp->slink == 0) {
            kmem_free((void *)cnp, sizeof(*cnp));
            return ENOMEM;
        }
        cnp->slink->parent = *parent;
        if (name != 0) {
            (void)strncpy(cnp->slink->name, name,
                          sizeof(cnp->slink->name) - 1);
            cnp->slink->name[sizeof(cnp->slink->name) - 1] = '\0';
        }
    }

    vp = CDFSTOV(cnp);
    VN_INIT(vp, vfsp, cdfs_vtype(n->kind), (dev_t)0);
    vp->v_op = &cdfs_vnodeops;
    vp->v_data = (caddr_t)cnp;
    vp->v_flag |= VNOMAP;

    cnp->hash_next = cvfs->hash[h];
    cvfs->hash[h] = cnp;

    *vpp = vp;
    return 0;
}

void cdfs_node_inactive(struct vnode *vp)
{
    struct cdfs_vfs  *cvfs = VFSTOCDFS(vp->v_vfsp);
    struct cdfsnode  *cnp = VTOCDFS(vp);
    struct cdfsnode **pp;
    unsigned int      h;

    h = cdfs_hash(cnp->node.extent.lba);

    for (pp = &cvfs->hash[h]; *pp != 0; pp = &(*pp)->hash_next) {
        if (*pp == cnp) {
            *pp = cnp->hash_next;
            break;
        }
    }

    if (cnp->slink != 0)
        kmem_free((void *)cnp->slink, sizeof(struct cdfs_symlink));
    kmem_free((void *)cnp, sizeof(*cnp));
}

#endif /* ODFS_PLATFORM_AMIX_KERNEL */
