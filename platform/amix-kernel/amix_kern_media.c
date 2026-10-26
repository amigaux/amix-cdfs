/*
 * amix_kern_media.c — AMIX (SVR4/m68k) IN-KERNEL SCSI optical media backend.
 *
 * Serves 2048-byte CD logical blocks off a REAL optical drive from inside the
 * kernel, by issuing SCSI CDBs straight down the SVR4 SCSI path. It is the
 * in-kernel twin of platform/amix/amix_scsi_media.c: the CDB-building logic
 * (READ(10)/0x28, READ CAPACITY(10)/0x25, INQUIRY/0x12) is identical, but the
 * TRANSPORT is swapped. Userspace fills a `struct sdcom` and hands it to the
 * kernel via ioctl('GSIO', /dev/scsi); here we fill the same `struct sdcom`
 * and call sdqueue() directly, then sleep until the driver's completion
 * interrupt fires. Because the kernel DMAs to its own buffer, the 1024-byte
 * gsioctl bounce-buffer clamp that blocks the userspace path does NOT apply.
 *
 * The wait mirrors scsi.c:gsioctl (sdqueue(); sdspl(); until(done) sleep();
 * spl0()) but WITHOUT gsioctl's copyin/static-iobuf/copyout: each request
 * carries its own `done` flag and sleep channel, never gsioctl's file-static
 * ones, so concurrent mounts do not collide. Each request is a per-call
 * kmem-heap allocation — globally mapped, NEVER a stack local, because an
 * interrupt-completing HBA driver dereferences it in arbitrary process
 * context (the amix_kern_req_t contract comment below has the full story).
 * The completion handler recovers its request from the sdcom pointer the
 * driver passes to (*cp->intr)(cp), relying on the sdcom being the first
 * member of the request struct.
 *
 * `struct sdcom` + SDCARDS/SDUNITS come verbatim from the cross sysroot's
 * kernel headers (usr/sys/amiga/alien/{rico.h,sd.h}); the ABI is never hand-
 * transcribed. Those headers are included LAST because rico.h defines
 * aggressive short-name macros (bool/uint/uchar/and/or/not/...).
 *
 * PROVIDER-FOUNDATION SCOPE: this file must COMPILE and LINK as part of the
 * relocatable kernel object. sdqueue()/vtop()/sleep()/wakeup() stay undefined
 * externs resolved at the final kernel link; spl0/spl2 (sdspl) inline from
 * <sys/inline.h>, so they emit no external symbol at all. The transport is not
 * exercised until the on-box bench.
 *
 * Written in C89 for the period AMIX cross compiler (GCC 2.7.2.3):
 * declarations at block scope, no C99 for-init, no mixed declarations.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "odfs/media.h"
#include "odfs/alloc.h"
#include "amix_kern_media.h"

/*
 * SVR4 kernel services. Included before the alien short-name macros so the
 * typedef'd uint/ushort/ulong in <sys/types.h> land BEFORE rico.h redefines
 * them as macros (matching amix_scsi_media.c's header ordering exactly).
 * <sys/systm.h> is deliberately NOT pulled in: it would drag its non-const
 * strcmp/strncpy prototypes (which clash with the compiler built-ins), and
 * all this backend needs from it is sleep()/wakeup(), declared K&R below —
 * exactly as the reference scsi.c:gsioctl relies on them.
 */
#include <sys/types.h>   /* caddr_t, paddr_t, uint (typedef) */
#include <sys/param.h>   /* PRIBIO                           */
#include <sys/immu.h>    /* NBPP: 2048 (68030) or 4096 (68040/060) */
#include <sys/cmn_err.h> /* cmn_err, CE_WARN                 */
#include <sys/inline.h>  /* _spl0/_spl2 inlines + spl0/spl2 macros   */

/* Kernel SCSI transport ABI — LAST: rico.h's bool/uint/uchar macros. */
#include "rico.h"        /* uchar/uint/bool, TRUE/FALSE               */
#include "sd.h"          /* struct sdcom, SDCARDS/SDUNITS, sdspl      */

/* A CD logical block is 2048 bytes (NOT the 512 of the hard-disk path). */
#define AMIX_KERN_CD_SECTOR_SIZE 2048u

/*
 * Per-request transfer cap, in BYTES. read_sectors chunks the requested
 * range so no single sdcom exceeds this.
 *
 * ONE SECTOR (2048). Hardware evidence: single-sector reads are correct (the PVD,
 * every mount read, and a direct cache-bypassing read all return good data), but a
 * multi-sector request — the block cache's read-ahead issues an 8-sector
 * (16 KB) DMA into one buffer, and file reads stream multiple sectors — comes
 * back as GARBAGE (the returned buffer held kernel text) and then wedges the
 * box. That is the signature of the SCSI HBA's DMA not spanning a transfer
 * larger than a single 2048-byte block here (a bounce/contiguity limit; this
 * board is the one that needed the a4091 super-DMAC bounce). Capping to one
 * sector keeps every DMA on the proven-good single-block path; the portable
 * core may still request many sectors — read_sectors just splits them into
 * one-block sdcoms. Raise this only once the HBA DMA limit is known good.
 */
#define AMIX_KERN_MAX_XFER AMIX_KERN_CD_SECTOR_SIZE

/*
 * The DMA page: the span a transfer target must not cross. NBPP is 2048 on
 * the 68030 line and 4096 on the 68040/68060 line. A target inside one
 * 2 KiB-aligned 2 KiB span lies inside one page on either line, so the checks
 * and aligned allocations below use 2048 on both: the value the static object
 * was built and proven with on both lines, whichever headers compile it (the
 * loadable-module build compiles once per line, see README.md). The
 * typedef refuses a page size that is not a multiple of it, and emits nothing.
 */
#define AMIX_KERN_DMA_PAGE 2048u
typedef char amix_kern_dma_page_check[(NBPP % AMIX_KERN_DMA_PAGE) == 0 ? 1 : -1];

/*
 * Kernel externs the headers above do not declare. Spelled K&R (no
 * prototype), matching the period kernel's own declarations, so the ABI is
 * whatever the kernel provides. Left undefined in the relocatable object.
 * spl0/spl2 (and thus sdspl) are NOT here: <sys/inline.h> supplies them as
 * static inline _spl0/_spl2, so the calls below inline rather than emit an
 * external symbol the whole-kernel link would have to resolve.
 */
struct proc;
extern void    sdqueue(/* struct sdcom * */);  /* scsi.c: enqueue a request  */
extern paddr_t vtop(/* caddr_t, struct proc * */); /* VA -> bus address      */
extern int     sleep(/* caddr_t, int */);      /* block on a channel         */
extern void    wakeup(/* caddr_t */);          /* wake sleepers on a channel */
extern char   *memcpy(/* void *, const void *, size_t */); /* kernel ml/exp;
                * K&R like the rest — this TU sees the sysroot <string.h>
                * (its scoped include path has no compat interceptor), whose
                * prototype clashes with <sys/inline.h>'s strlen, so it is
                * not included here */

typedef struct amix_kern_ctx {
    unsigned int card;
    unsigned int unit;
    uint32_t     sector_count;   /* READ CAPACITY last_lba + 1 */
} amix_kern_ctx_t;

/*
 * One in-flight request. `sc` MUST be the first member: the SCSI driver
 * completes a request by calling (*cp->intr)(cp) with the sdcom pointer, so
 * the completion handler casts that pointer straight back to this struct.
 *
 * LIFETIME/MAPPING CONTRACT (the remount-cycle panic): a request handed
 * to sdqueue() MUST live in GLOBALLY-MAPPED kernel memory — kmem heap or
 * statics — NEVER on the caller's kernel stack. On AMIX the kernel stack
 * lives in the per-process u-area window (a fixed virtual range, ~0x40001xxx,
 * REMAPPED at every context switch), and an interrupt-completing HBA driver
 * (a3091/a4091) delivers the completion from its ISR in WHATEVER process
 * context the interrupt happens to land in: while the submitter sleeps below,
 * another process is on-CPU, and the driver's `cp` — a u-area-window address —
 * then resolves into THAT process's stack. The ISR reads cp->next, writes
 * cp->okay/cp->status into the victim's stack, and jumps through a garbage
 * cp->intr (a3091intr case 0: `moveal %a2@(36),%a0; jsr %a0@`) — the
 * KERNEL FAULT psw=0x2200 wild-pc panic in an unrelated process (seen as
 * flopd blocked in fdopen; the card-0 repro). Every stock client obeys
 * this contract with permanent sdcoms (dd.c: ddtab[][].com; scsi.c gsioctl: a
 * file-static); the same class was proven once before on metal as an earlier
 * mount panic (z3660scsi's then timeout()-deferred completion dereferencing a
 * dead stack sdcom). A purely synchronous in-context driver (today's
 * z3660scsi) never dereferences cross-context — which is why card 1 on the
 * bench survived what card 0 (a3091, interrupt-completing) died of.
 */
typedef struct amix_kern_req {
    struct sdcom   sc;
    volatile int   done;
} amix_kern_req_t;

static const odfs_media_ops_t amix_kern_media_ops;

/* ---- transport: struct sdcom + sdqueue() + sleep loop ----------------- */

static void amix_kern_intr(cp)
    struct sdcom *cp;
{
    amix_kern_req_t *r = (amix_kern_req_t *)cp;  /* sc is first member */

    r->done = 1;
    wakeup((caddr_t)cp);
}

static odfs_err_t amix_kern_submit(amix_kern_ctx_t *ctx,
                                   const unsigned char *cdb,
                                   unsigned int cdb_len,
                                   int reading,
                                   void *data,
                                   unsigned int nbyte)
{
    /*
     * The request is allocated from the kmem heap, NEVER on this stack frame:
     * see the amix_kern_req_t contract comment above. The heap block is
     * mapped identically in every process context, so the HBA driver's queue
     * links and its (possibly foreign-context) completion ISR always
     * dereference the real request — a stack `req` here was the
     * remount-cycle wild-pc panic. The sleep loop below still keeps the
     * request alive for the whole transaction; it is freed only after the
     * completion handler has fired.
     */
    amix_kern_req_t *req;
    odfs_err_t       err;
    unsigned int     i;

    if (ctx == 0)
        return ODFS_ERR_IO;
    if (cdb_len > sizeof(req->sc.cdb))   /* sizeof: constant, req not read */
        return ODFS_ERR_INVAL;

    /*
     * DMA-contract invariant: the transfer must not cross a page boundary.
     *
     * sc.addr is a PHYSICAL address (vtop of `data`) and the HBA transfers
     * sc.nbyte bytes LINEARLY through physical memory from it. vtop()
     * translates exactly one page; SVR4 kmem/sptalloc memory is virtually
     * contiguous but PHYSICALLY SCATTERED (segkmem_alloc maps arbitrary
     * page_get frames), so any transfer that runs past the end of `data`'s
     * page writes into an UNRELATED physical page — a wild write into
     * whatever happens to own the physically-next frame (this exact bug
     * zeroed the head of cached binaries on the metal box). Translation
     * preserves the in-page offset, so checking the virtual offset is
     * equivalent to checking the physical one. Refuse loudly rather than
     * fire a wild DMA; every internal caller guarantees an in-page target
     * (page-aligned sector buffers, or the bounce in read_sectors).
     */
    if (((unsigned long)data & (AMIX_KERN_DMA_PAGE - 1)) + nbyte >
        (unsigned long)AMIX_KERN_DMA_PAGE) {
        cmn_err(CE_WARN,
                "cdfs: refused DMA crossing a page: addr 0x%x nbyte %d",
                (unsigned int)(unsigned long)data, (int)nbyte);
        return ODFS_ERR_INVAL;
    }

    /* odfs_calloc = kmem_zalloc(KM_SLEEP): globally-mapped, zeroed — the
     * whole-sdcom zeroing gsioctl does is covered by the calloc. Process
     * context only, so blocking for memory is fine (same as the read bounce). */
    req = (amix_kern_req_t *)odfs_calloc(1, sizeof(*req));
    if (req == 0)
        return ODFS_ERR_NOMEM;

    req->sc.card    = ctx->card;
    req->sc.unit    = ctx->unit;
    req->sc.reading = (bool)(reading ? TRUE : FALSE);
    /* Bus address for the driver's DMA. Identity-mapped on AMIX, but run it
     * through vtop() anyway so the value is correct if that ever changes. */
    req->sc.addr    = (caddr_t)vtop(data, (struct proc *)0);
    req->sc.nbyte   = nbyte;
    for (i = 0; i < cdb_len; i++)
        req->sc.cdb[i] = (uchar)cdb[i];
    req->sc.intr = amix_kern_intr;

    /*
     * Raise to spl2 (mask the IPL2 clock/SCSI interrupt) BEFORE issuing, so
     * the whole synchronous mailbox transaction is atomic w.r.t. the clock.
     * The z3660 driver completes disk I/O from a timeout() callout in the
     * clock ISR, which re-enters sdqueue() (via dd.c's disk-completion
     * startio) and would nest a second transaction on the single shared HBA
     * mailbox + bounce window mid-transfer, scrambling in-flight state.
     * Raising the guard first makes this match dd.c's proven-safe ordering
     * (ddstrategy: sdspl() before startio()). sleep() drops spl to admit the
     * completion and restores spl2 on wake, so it also blocks the interrupt
     * across the test-and-sleep and cannot lose a wakeup (sdspl == spl2; drop
     * with spl0). Do NOT reorder sdspl() back below sdqueue().
     */
    sdspl();
    sdqueue(&req->sc);
    while (!req->done)
        sleep((caddr_t)&req->sc, PRIBIO);
    spl0();

    /* Completion has fired (done set by amix_kern_intr); the driver holds no
     * live reference — its queue head advanced past this sdcom before the
     * intr call, and the dangling per-unit tail pointer the stock queue
     * discipline leaves behind is only ever read under a non-empty queue,
     * which the next enqueue re-seeds. Safe to free. */
    err = (!req->sc.okay || req->sc.status != 0) ? ODFS_ERR_IO : ODFS_OK;
    odfs_free(req);
    return err;
}

/* ---- media ops -------------------------------------------------------- */

static odfs_err_t amix_kern_read_sectors(void *ctx, uint32_t lba,
                                         uint32_t count, void *buf)
{
    amix_kern_ctx_t *c = (amix_kern_ctx_t *)ctx;
    unsigned char   *out = (unsigned char *)buf;
    unsigned char   *bounce = 0;
    uint32_t         max_blocks = AMIX_KERN_MAX_XFER / AMIX_KERN_CD_SECTOR_SIZE;
    odfs_err_t       err = ODFS_OK;

    if (c == 0)
        return ODFS_ERR_IO;
    if (count == 0)
        return ODFS_OK;

    while (count != 0) {
        uint32_t       chunk = (count < max_blocks) ? count : max_blocks;
        unsigned int   nbyte = (unsigned int)(chunk * AMIX_KERN_CD_SECTOR_SIZE);
        unsigned char *dst = out;
        unsigned char  cdb[10];

        /*
         * Page-aligned targets (the core's sector-aligned cache buffers,
         * and page-aligned callers like cdfs_read's RDBUF on sector-aligned
         * file offsets) DMA directly: one 2048-byte sector into exactly one
         * 2048-byte page. An UNALIGNED target (a caller streaming full
         * sectors into the middle of its own buffer — e.g. cdfs_read at a
         * non-sector-aligned uio_offset) would make the linear physical
         * transfer cross a page into an unrelated frame, so it goes through
         * a page-aligned bounce sector and a memcpy instead. Allocated
         * per call (lazily, first unaligned chunk), NOT shared per mount:
         * the submit sleep is a reschedule point and concurrent readers
         * of one mount must not share a bounce.
         */
        if (((unsigned long)out & (AMIX_KERN_DMA_PAGE - 1)) != 0) {
            if (bounce == 0) {
                bounce = (unsigned char *)
                    odfs_malloc_aligned((size_t)AMIX_KERN_MAX_XFER,
                                        (size_t)AMIX_KERN_DMA_PAGE);
                if (bounce == 0)
                    return ODFS_ERR_NOMEM;
            }
            dst = bounce;
        }

        /* READ(10): opcode, LBA in cdb[2..5], block count in cdb[7..8]. */
        cdb[0] = 0x28;
        cdb[1] = 0;
        cdb[2] = (unsigned char)((lba >> 24) & 0xFF);
        cdb[3] = (unsigned char)((lba >> 16) & 0xFF);
        cdb[4] = (unsigned char)((lba >>  8) & 0xFF);
        cdb[5] = (unsigned char)( lba        & 0xFF);
        cdb[6] = 0;
        cdb[7] = (unsigned char)((chunk >> 8) & 0xFF);
        cdb[8] = (unsigned char)( chunk       & 0xFF);
        cdb[9] = 0;

        /* Data length is blocks * 2048 (CD), NOT * 512. */
        err = amix_kern_submit(c, cdb, 10, 1, dst, nbyte);
        if (err != ODFS_OK)
            break;

        if (dst == bounce)
            memcpy(out, bounce, (size_t)nbyte);

        out   += nbyte;
        lba   += chunk;
        count -= chunk;
    }

    if (bounce)
        odfs_free_aligned(bounce);
    return err;
}

static uint32_t amix_kern_sector_size(void *ctx)
{
    (void)ctx;
    return AMIX_KERN_CD_SECTOR_SIZE;
}

static uint32_t amix_kern_sector_count(void *ctx)
{
    amix_kern_ctx_t *c = (amix_kern_ctx_t *)ctx;
    return c->sector_count;
}

static void amix_kern_close(void *ctx)
{
    amix_kern_ctx_t *c = (amix_kern_ctx_t *)ctx;

    if (c)
        odfs_free(c);
}

static const odfs_media_ops_t amix_kern_media_ops = {
    amix_kern_read_sectors, /* read_sectors                               */
    amix_kern_sector_size,  /* sector_size                                */
    amix_kern_sector_count, /* sector_count                               */
    0,                      /* read_toc              (NULL -> UNSUPPORTED) */
    0,                      /* read_last_session_lba (NULL -> UNSUPPORTED) */
    0,                      /* read_audio            (NULL -> UNSUPPORTED) */
    0,                      /* read_cdtext           (NULL -> UNSUPPORTED) */
    amix_kern_close         /* close                                      */
};

/* ---- open-time SCSI probes -------------------------------------------- */

/* Big-endian 32-bit load from a READ CAPACITY response field. */
static uint32_t amix_kern_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) | ((uint32_t)p[3]);
}

/*
 * The probe responses (INQUIRY 36 bytes, READ CAPACITY 8 bytes) are DMA
 * targets too, so they must satisfy the same page-cross invariant as the
 * sector reads. A stack buffer sits at an arbitrary in-page offset and can
 * straddle a page boundary — and DMAing into the kernel stack is fragile
 * anyway — so each probe allocates a page-aligned scratch page instead
 * (mount-time only; the cost is irrelevant).
 */

/* INQUIRY (0x12): confirm this really is a CD-ROM (peripheral type 0x05). */
static odfs_err_t amix_kern_check_inquiry(amix_kern_ctx_t *c)
{
    unsigned char  cdb[6];
    unsigned char *data;
    odfs_err_t     err;

    data = (unsigned char *)odfs_malloc_aligned((size_t)AMIX_KERN_DMA_PAGE,
                                                    (size_t)AMIX_KERN_DMA_PAGE);
    if (data == 0)
        return ODFS_ERR_NOMEM;

    cdb[0] = 0x12;   /* INQUIRY            */
    cdb[1] = 0;      /* EVPD = 0           */
    cdb[2] = 0;      /* page code          */
    cdb[3] = 0;
    cdb[4] = 36;     /* allocation length  */
    cdb[5] = 0;      /* control            */

    err = amix_kern_submit(c, cdb, 6, 1, data, 36u);

    /*
     * Peripheral device type is the low 5 bits of byte 0. 0x05 = CD-ROM.
     *
     * A target that ANSWERS but is not type 0x05 is its own verdict: the
     * address names a real device, it is simply the wrong kind (a disk, a
     * tape). It is neither an I/O failure nor an empty card slot, so it gets
     * its own error code rather than the generic UNSUPPORTED — cdfs_errno()
     * maps it to ENOTTY, which no other cdfs path produces.
     */
    if (err == ODFS_OK && (data[0] & 0x1f) != 0x05)
        err = ODFS_ERR_NOT_OPTICAL;

    odfs_free_aligned(data);
    return err;
}

/* READ CAPACITY(10) (0x25): last LBA + block length; require 2048-byte blocks. */
static odfs_err_t amix_kern_read_capacity(amix_kern_ctx_t *c)
{
    unsigned char  cdb[10];
    unsigned char *data;
    uint32_t       last_lba;
    uint32_t       block_len;
    odfs_err_t     err;
    unsigned int   i;

    data = (unsigned char *)odfs_malloc_aligned((size_t)AMIX_KERN_DMA_PAGE,
                                                    (size_t)AMIX_KERN_DMA_PAGE);
    if (data == 0)
        return ODFS_ERR_NOMEM;

    for (i = 0; i < (unsigned int)sizeof(cdb); i++)
        cdb[i] = 0;
    cdb[0] = 0x25;   /* READ CAPACITY(10); LBA 0, PMI 0 */

    err = amix_kern_submit(c, cdb, 10, 1, data, 8u);
    if (err != ODFS_OK) {
        odfs_free_aligned(data);
        return err;
    }

    last_lba  = amix_kern_be32(&data[0]);
    block_len = amix_kern_be32(&data[4]);
    odfs_free_aligned(data);

    /* 0xFFFFFFFF means the capacity overflows READ CAPACITY(10). */
    if (last_lba == 0xFFFFFFFFu)
        return ODFS_ERR_UNSUPPORTED;
    if (block_len != AMIX_KERN_CD_SECTOR_SIZE)
        return ODFS_ERR_UNSUPPORTED;

    c->sector_count = last_lba + 1u;
    return ODFS_OK;
}

/* ---- open ------------------------------------------------------------- */

odfs_err_t odfs_kern_media_open_scsi(unsigned int card, unsigned int unit,
                                     odfs_media_t *out)
{
    amix_kern_ctx_t *c;
    odfs_err_t       err;
    int              rc;

    if (out == 0)
        return ODFS_ERR_INVAL;
    if (card >= (unsigned int)SDCARDS || unit >= (unsigned int)SDUNITS)
        return ODFS_ERR_INVAL;

    /*
     * Validate + init the card once. sdopen() runs the controller probe that
     * populates the queue table sdqueue() dispatches through, and is
     * idempotent, so a single call at open is enough. Declared in sd.h.
     *
     * A nonzero return means the card slot holds NO REGISTERED CONTROLLER:
     * the address names nothing, as opposed to naming a drive that failed to
     * talk. Reporting that as ODFS_ERR_IO (errno 5) hid it behind the same
     * EIO as a genuine transport failure and a bad-format disc, and cost
     * three separate investigations — the stock kernel unregisters the a3091
     * once memory exceeds 64 MB (support.c), so an empty card slot is a
     * routine configuration outcome that must be readable straight off
     * mount(1M)'s message. ODFS_ERR_NO_DEVICE -> ENXIO, and the console
     * warning names the card so the cause is on the log even when the caller
     * discards the errno.
     */
    rc = sdopen(card);
    if (rc != 0) {
        cmn_err(CE_WARN, "cdfs: card %d not registered (sdopen=%d)",
                (int)card, rc);
        return ODFS_ERR_NO_DEVICE;
    }

    c = (amix_kern_ctx_t *)odfs_calloc(1, sizeof(*c));
    if (c == 0)
        return ODFS_ERR_NOMEM;

    c->card         = card;
    c->unit         = unit;
    c->sector_count = 0;

    err = amix_kern_check_inquiry(c);
    if (err != ODFS_OK) {
        odfs_free(c);
        return err;
    }

    err = amix_kern_read_capacity(c);
    if (err != ODFS_OK) {
        odfs_free(c);
        return err;
    }

    out->ops = &amix_kern_media_ops;
    out->ctx = c;
    return ODFS_OK;
}
