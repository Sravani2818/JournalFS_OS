/*
 * journal.c -- Layer 3/4 support: the journal region and the transaction
 *              manager (write ahead logging).
 *
 * Journal region geometry
 * -----------------------
 * The journal region is a circular array of JFS_JOURNAL_BLOCKS blocks.  A
 * "window" [journal_start, journal_start + journal_used) holds the records
 * that are still needed for recovery.  Records are written at the end of the
 * window; the window shrinks from the front when a checkpoint makes earlier
 * transactions unnecessary.  The window bookkeeping lives in the checkpoint
 * record, and it is *re-derived by scanning* at mount time, so a torn
 * checkpoint cannot make recovery read outside the region.
 *
 * Transaction protocol
 * --------------------
 *   begin   -> ops are kept in memory, nothing is written
 *   commit  -> OP record(s) appended, fsync, COMMIT record appended, fsync
 *   apply   -> redo operations appended to the log-structured log region
 *   end     -> journal window advanced, checkpoint written and fsynced
 *
 * The only durability barrier that matters to a client is the second fsync
 * in `commit`: after it returns, the transaction will survive a crash even
 * if the process dies before `apply` runs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "checksum.h"
#include "crash.h"
#include "journal.h"
#include "journalfs.h"
#include "storage.h"
#include "util.h"

/* Largest payload a single OP journal record may carry.  Bigger transactions
 * are split into several OP records (the COMMIT record still covers them all
 * because recovery groups contiguous records by transaction id). */
#define JFS_JOURNAL_REC_MAX_PAYLOAD (96u * 1024u)

/* ------------------------------------------------------------------ */
/* record header helpers                                               */
/* ------------------------------------------------------------------ */

void jfs_log_rec_finalize(jfs_log_rec_hdr *h)
{
    h->hdr_crc = 0;
    h->hdr_crc = jfs_crc_struct(h, sizeof(*h), offsetof(jfs_log_rec_hdr, hdr_crc));
}

bool jfs_log_rec_check(const jfs_log_rec_hdr *h)
{
    jfs_log_rec_hdr copy = *h;
    uint32_t expect;

    if (h->hdr_crc == 0)
        return false;
    copy.hdr_crc = 0;
    expect = jfs_crc_struct(&copy, sizeof(copy), offsetof(jfs_log_rec_hdr, hdr_crc));
    return expect == h->hdr_crc;
}

void jfs_journal_rec_finalize(jfs_journal_rec_hdr *h)
{
    h->hdr_crc = 0;
    h->hdr_crc = jfs_crc_struct(h, sizeof(*h), offsetof(jfs_journal_rec_hdr, hdr_crc));
}

bool jfs_journal_rec_check(const jfs_journal_rec_hdr *h)
{
    jfs_journal_rec_hdr copy = *h;
    uint32_t expect;

    if (h->hdr_crc == 0)
        return false;
    copy.hdr_crc = 0;
    expect = jfs_crc_struct(&copy, sizeof(copy), offsetof(jfs_journal_rec_hdr, hdr_crc));
    return expect == h->hdr_crc;
}

/* ------------------------------------------------------------------ */
/* journal region I/O                                                  */
/* ------------------------------------------------------------------ */

uint64_t jfs_journal_block_off(const struct jfs_fs *fs, uint64_t index)
{
    return (fs->sb.journal_block + index) * JFS_BLOCK_SIZE;
}

/* Window-relative index -> absolute journal block index (with wraparound). */
static uint64_t win_to_rel(const struct jfs_fs *fs, uint64_t win_index)
{
    return (fs->ckpt.journal_start + win_index) % fs->sb.journal_blocks;
}

bool jfs_journal_room(const struct jfs_fs *fs, uint32_t blocks)
{
    if (fs->ckpt.journal_used > fs->sb.journal_blocks)
        return false;
    return blocks <= (fs->sb.journal_blocks - fs->ckpt.journal_used);
}

void jfs_journal_reset(struct jfs_fs *fs)
{
    fs->ckpt.journal_start = 0;
    fs->ckpt.journal_used = 0;
    fs->journal_head = 0;
    fs->journal_lsn = 0;
}

int jfs_journal_append(struct jfs_fs *fs, const jfs_journal_rec_hdr *hdr,
                       const void *payload, size_t payload_len)
{
    uint64_t total = sizeof(*hdr) + payload_len;
    uint32_t nblocks = (uint32_t)((total + JFS_BLOCK_SIZE - 1u) / JFS_BLOCK_SIZE);
    unsigned char *stage;
    size_t stage_bytes;
    uint32_t b;

    if (nblocks == 0)
        return JFS_ERR_INVAL;
    if (!jfs_journal_room(fs, nblocks))
        return JFS_ERR_JFULL;

    stage_bytes = (size_t)nblocks * JFS_BLOCK_SIZE;
    stage = (unsigned char *)calloc(1, stage_bytes);
    if (!stage)
        return JFS_ERR_NOMEM;
    memcpy(stage, hdr, sizeof(*hdr));
    if (payload_len)
        memcpy(stage + sizeof(*hdr), payload, payload_len);

    for (b = 0; b < nblocks; b++) {
        uint64_t rel = win_to_rel(fs, fs->ckpt.journal_used + b);
        ssize_t n = jfs_pwrite_full(fs->fd, stage + (size_t)b * JFS_BLOCK_SIZE,
                                    JFS_BLOCK_SIZE, (off_t)jfs_journal_block_off(fs, rel));
        if (n < 0 || (size_t)n != JFS_BLOCK_SIZE) {
            free(stage);
            return JFS_ERR_IO;
        }
        fs->stats.bytes_written += JFS_BLOCK_SIZE;
    }
    free(stage);

    fs->ckpt.journal_used += nblocks;
    fs->journal_head = fs->ckpt.journal_used;
    fs->journal_lsn = hdr->lsn;
    return JFS_OK;
}

/*
 * Free the leading journal records belonging to transactions with
 * txid <= upto.  Only called once a checkpoint that names the new window has
 * been written AND synced, which is what makes reusing the blocks safe.
 * `start` / `used` are updated in place.
 */
int jfs_journal_window_plan(struct jfs_fs *fs, uint64_t upto,
                            uint64_t *start, uint64_t *used, uint64_t *freed)
{
    uint64_t s = *start;
    uint64_t u = *used;
    uint64_t f = 0;
    uint64_t guard = 0;
    uint64_t last_lsn = 0;

    while (u > 0 && guard++ < fs->sb.journal_blocks) {
        jfs_journal_rec_hdr hdr;
        ssize_t n;

        n = jfs_pread_full(fs->fd, &hdr, sizeof(hdr),
                           (off_t)jfs_journal_block_off(fs, s));
        if (n < 0)
            return JFS_ERR_IO;
        if ((size_t)n != sizeof(hdr))
            break;
        if (hdr.magic != JFS_JREC_MAGIC || hdr.version != JFS_FORMAT_VERSION ||
            hdr.image_id != fs->sb.image_id || !jfs_journal_rec_check(&hdr))
            break;                      /* stop at the first unusable record */
        if (hdr.txid > upto)
            break;
        if (hdr.nblocks == 0 || hdr.nblocks > u)
            break;
        if (f > 0 && hdr.lsn <= last_lsn)
            break;                      /* residue from an earlier lap */
        last_lsn = hdr.lsn;
        s = (s + hdr.nblocks) % fs->sb.journal_blocks;
        u -= hdr.nblocks;
        f += hdr.nblocks;
    }
    *start = s;
    *used = u;
    if (freed)
        *freed = f;
    return JFS_OK;
}

int jfs_journal_checkpoint_advance(struct jfs_fs *fs, uint64_t upto, uint64_t *freed_blocks)
{
    uint64_t start = fs->ckpt.journal_start;
    uint64_t used = fs->ckpt.journal_used;
    uint64_t freed = 0;
    int rc = jfs_journal_window_plan(fs, upto, &start, &used, &freed);

    if (rc != JFS_OK)
        return rc;
    fs->ckpt.journal_start = start;
    fs->ckpt.journal_used = used;
    fs->journal_head = used;
    fs->stats.journal_blocks_freed += freed;
    if (freed_blocks)
        *freed_blocks = freed;
    return JFS_OK;
}

/*
 * Read a record header at a window-relative index and check that it is
 * structurally usable.  Returns JFS_ERR_CORRUPT for anything that is not a
 * record of this filesystem; the scan routines treat that as "end of data".
 */
static int scan_header(const struct jfs_fs *fs, uint64_t win_index,
                       jfs_journal_rec_hdr *out, uint64_t *last_lsn, bool *any)
{
    ssize_t n = jfs_pread_full(fs->fd, out, sizeof(*out),
                               (off_t)jfs_journal_block_off(fs, win_to_rel(fs, win_index)));
    if (n < 0)
        return JFS_ERR_IO;
    if ((size_t)n != sizeof(*out))
        return JFS_ERR_CORRUPT;
    if (out->magic != JFS_JREC_MAGIC || out->version != JFS_FORMAT_VERSION ||
        out->image_id != fs->sb.image_id || !jfs_journal_rec_check(out))
        return JFS_ERR_CORRUPT;
    if (out->nblocks == 0 || out->nblocks > fs->sb.journal_blocks)
        return JFS_ERR_CORRUPT;
    if (*any && out->lsn <= *last_lsn)
        return JFS_ERR_CORRUPT;         /* sequence went backwards */
    *last_lsn = out->lsn;
    *any = true;
    return JFS_OK;
}

/*
 * Re-derive the journal window after mounting (or for `inspect`).
 *
 * The window is "the records that recovery may still have to replay".  It is
 * rebuilt in two steps:
 *
 *   1. the prefix that the durable checkpoint names: [journal_start,
 *      journal_start + journal_used).  Its length is authoritative -- only a
 *      checkpoint makes records reclaimable -- but every header in it is
 *      re-validated so that a damaged block shrinks the window instead of
 *      making the mount read outside the region.
 *
 *   2. the records written *after* that checkpoint was taken: a crash can
 *      leave a committed transaction in the journal that no checkpoint
 *      mentions yet (that is the whole point of write ahead logging), so the
 *      scan continues past the checkpointed end while it sees records whose
 *      txid is newer than `applied_txid`.
 *
 * Step 2 is where a naive scan goes wrong.  Freeing a record is logical: the
 * window is advanced past it, but its bytes stay on the medium until a newer
 * record is written over them.  Once the ring has wrapped, that residue looks
 * exactly like a live window (correct magic, correct checksum, increasing
 * sequence numbers), so a scan without a rule to stop it would resurrect
 * already-applied transactions and hand the append pointer blocks that are
 * still in use.  The rule used here is the transaction id: a record is only
 * ever freed once its txid is <= the applied txid of a checkpoint, so any
 * record with a txid that is *not* newer than this checkpoint's applied txid
 * is residue, and the scan stops there.
 */
/*
 * Look for a still-valid record further along the ring.  Used only to tell
 * "the window ends here" apart from "the window is damaged here": if a valid
 * record of a transaction that is newer than the checkpoint still exists past
 * the point where the walk stopped, then something in between was overwritten
 * or destroyed, and the records after it cannot be replayed.  The caller
 * reports that instead of silently mounting an older state.
 */
static bool damaged_window_probe(const struct jfs_fs *fs, uint64_t from,
                                 uint64_t last_lsn, bool any,
                                 uint64_t *block_out)
{
    uint64_t blocks = fs->sb.journal_blocks;
    uint64_t at;

    for (at = from; at < blocks; at++) {
        jfs_journal_rec_hdr hdr;
        ssize_t n = jfs_pread_full(fs->fd, &hdr, sizeof(hdr),
                                   (off_t)jfs_journal_block_off(fs, win_to_rel(fs, at)));
        if (n < 0)
            return false;
        if ((size_t)n != sizeof(hdr))
            continue;
        if (hdr.magic != JFS_JREC_MAGIC || hdr.version != JFS_FORMAT_VERSION ||
            hdr.image_id != fs->sb.image_id || !jfs_journal_rec_check(&hdr))
            continue;
        if (hdr.nblocks == 0 || hdr.nblocks > blocks)
            continue;
        if (hdr.txid <= fs->ckpt.applied_txid)
            continue;                   /* reclaimed residue, not damage */
        if (any && hdr.lsn <= last_lsn)
            continue;                   /* older than the window: residue */
        if (block_out)
            *block_out = win_to_rel(fs, at);
        return true;
    }
    return false;
}

int jfs_journal_rescan(struct jfs_fs *fs)
{
    uint64_t blocks = fs->sb.journal_blocks;
    uint64_t used = fs->ckpt.journal_used;
    uint64_t declared = fs->ckpt.journal_used;
    uint64_t last_lsn = 0;
    uint64_t walk;
    uint64_t damaged_at = 0;
    bool any = false;
    bool truncated = false;

    fs->journal_truncated = false;
    fs->journal_damaged_block = 0;

    if (used > blocks)
        used = blocks;                  /* damaged checkpoint: clamp */
    if (declared > blocks)
        declared = blocks;

    /* 1. validate the checkpointed prefix */
    for (walk = 0; walk < used; ) {
        jfs_journal_rec_hdr hdr;
        int rc = scan_header(fs, walk, &hdr, &last_lsn, &any);
        if (rc == JFS_ERR_IO)
            return rc;
        if (rc != JFS_OK || walk + hdr.nblocks > used) {
            used = walk;                /* stop at the first damaged record */
            truncated = true;
            damaged_at = win_to_rel(fs, walk);
            break;
        }
        walk += hdr.nblocks;
    }

    /* 2. extend over transactions that committed after that checkpoint */
    for (walk = used; walk < blocks; ) {
        jfs_journal_rec_hdr hdr;
        int rc = scan_header(fs, walk, &hdr, &last_lsn, &any);
        if (rc == JFS_ERR_IO)
            return rc;
        if (rc != JFS_OK) {
            /* Only damage matters here: stopping on residue (a record of an
             * applied transaction) is the normal end of the window. */
            uint64_t at = 0;
            if (damaged_window_probe(fs, walk + 1, last_lsn, any, &at)) {
                truncated = true;
                damaged_at = at;
            }
            break;
        }
        if (hdr.txid <= fs->ckpt.applied_txid)
            break;                      /* residue of an applied transaction */
        if (walk + hdr.nblocks > blocks)
            break;
        walk += hdr.nblocks;
    }

    (void)declared;
    fs->journal_truncated = truncated;
    fs->journal_damaged_block = damaged_at;
    fs->ckpt.journal_used = walk;
    fs->journal_head = walk;

    /*
     * Continue the record sequence where the records found in the window left
     * off.  Records of an earlier session stay in the window until a
     * checkpoint accounts for them, so a session that started counting from
     * zero would write records *older* than records that are already there --
     * and the sequence check used to tell live records apart from the residue
     * of an earlier lap would then report the new records as damage.
     */
    if (any)
        fs->journal_lsn = last_lsn;
    return JFS_OK;
}

/* Read one journal record header at a window index. */
int jfs_journal_read_hdr(struct jfs_fs *fs, uint64_t win_index, jfs_journal_rec_hdr *out)
{
    ssize_t n = jfs_pread_full(fs->fd, out, sizeof(*out),
                               (off_t)jfs_journal_block_off(fs, win_to_rel(fs, win_index)));
    if (n < 0)
        return JFS_ERR_IO;
    if ((size_t)n != sizeof(*out))
        return JFS_ERR_TRUNCATED;
    if (out->magic != JFS_JREC_MAGIC)
        return JFS_ERR_CORRUPT;
    if (out->version != JFS_FORMAT_VERSION)
        return JFS_ERR_CORRUPT;
    if (out->image_id != fs->sb.image_id)
        return JFS_ERR_CORRUPT;
    if (!jfs_journal_rec_check(out))
        return JFS_ERR_CORRUPT;
    if (out->nblocks == 0 ||
        out->nblocks > (uint32_t)(fs->sb.journal_blocks * JFS_BLOCK_SIZE / JFS_BLOCK_SIZE))
        return JFS_ERR_CORRUPT;
    if (out->payload_len > (uint32_t)(out->nblocks * JFS_BLOCK_SIZE - sizeof(*out)))
        return JFS_ERR_CORRUPT;
    return JFS_OK;
}

/* Read the payload of a journal record (validating its CRC). */
int jfs_journal_read_payload(struct jfs_fs *fs, uint64_t win_index,
                             const jfs_journal_rec_hdr *hdr, void *buf, size_t bufsz)
{
    uint64_t off;
    ssize_t n;

    if (hdr->payload_len > bufsz)
        return JFS_ERR_INVAL;
    off = jfs_journal_block_off(fs, win_to_rel(fs, win_index)) + sizeof(*hdr);
    n = jfs_pread_full(fs->fd, buf, hdr->payload_len, (off_t)off);
    if (n < 0)
        return JFS_ERR_IO;
    if ((size_t)n != hdr->payload_len)
        return JFS_ERR_TRUNCATED;
    if (hdr->payload_len && jfs_crc32c(buf, hdr->payload_len) != hdr->payload_crc)
        return JFS_ERR_CORRUPT;
    return JFS_OK;
}
