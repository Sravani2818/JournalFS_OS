/*
 * transaction.c -- transaction lifecycle (Layer 3).
 *
 *   jfs_txn_begin   allocate a transaction id, keep the intended changes in
 *                   memory only (nothing reaches the image)
 *   jfs_txn_commit  write OP record(s) -> fsync -> write COMMIT -> fsync.
 *                   The second fsync is the durability barrier: after it the
 *                   transaction survives a crash even if `apply` never runs.
 *   jfs_txn_apply   redo the operations into the log-structured log region
 *                   (new versions appended; old versions stay put)
 *   jfs_txn_end     advance the journal window, write + fsync the checkpoint
 *
 * Recovery re-uses jfs_txn_apply() with transactions rebuilt from the
 * journal, so the normal path and the recovery path cannot drift apart.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "checksum.h"
#include "crash.h"
#include "inode.h"
#include "journal.h"
#include "journalfs.h"
#include "storage.h"
#include "util.h"

/* Largest payload a single OP journal record may carry.  Bigger transactions
 * are split into several OP records; recovery groups them by transaction id. */
#define JFS_JOURNAL_REC_MAX_PAYLOAD (96u * 1024u)

/* ------------------------------------------------------------------ */
/* building a transaction                                              */
/* ------------------------------------------------------------------ */

static int txn_reserve(jfs_txn *tx, uint32_t extra_ops)
{
    if (tx->nops + extra_ops > tx->cap) {
        uint32_t ncap = tx->cap ? tx->cap * 2 : 8;
        jfs_redo_op *np;
        while (ncap < tx->nops + extra_ops)
            ncap *= 2;
        np = (jfs_redo_op *)realloc(tx->ops, (size_t)ncap * sizeof(jfs_redo_op));
        if (!np)
            return JFS_ERR_NOMEM;
        tx->ops = np;
        tx->cap = ncap;
    }
    return JFS_OK;
}

int jfs_txn_begin(struct jfs_fs *fs, jfs_txn **out)
{
    jfs_txn *tx;

    if (!fs || !fs->mounted || !out)
        return JFS_ERR_NOTMOUNTED;
    tx = (jfs_txn *)calloc(1, sizeof(*tx));
    if (!tx)
        return JFS_ERR_NOMEM;
    tx->fs = fs;
    tx->txid = fs->next_txid++;
    tx->active = true;
    *out = tx;
    return JFS_OK;
}

void jfs_txn_free(jfs_txn *tx)
{
    uint32_t i;

    if (!tx)
        return;
    for (i = 0; i < tx->nops; i++)
        free(tx->ops[i].payload);
    free(tx->ops);
    free(tx);
}

void jfs_txn_abort(jfs_txn *tx)
{
    jfs_txn_free(tx);
}

int jfs_txn_add_raw(jfs_txn *tx, uint16_t op, uint32_t ino, uint32_t index,
                    const void *payload, uint32_t len)
{
    jfs_redo_op *o;
    int rc;

    if (!tx)
        return JFS_ERR_INVAL;
    rc = txn_reserve(tx, 1);
    if (rc != JFS_OK)
        return rc;
    o = &tx->ops[tx->nops];
    memset(o, 0, sizeof(*o));
    o->op = op;
    o->ino = ino;
    o->index = index;
    o->length = len;
    if (len) {
        o->payload = (unsigned char *)malloc(len);
        if (!o->payload)
            return JFS_ERR_NOMEM;
        memcpy(o->payload, payload, len);
    }
    tx->nops++;
    tx->payload_bytes += sizeof(jfs_redo_op_hdr) + len;
    return JFS_OK;
}

int jfs_txn_add_inode(jfs_txn *tx, const jfs_inode *inode)
{
    return jfs_txn_add_raw(tx, JFS_OP_INODE, inode->ino, 0, inode, sizeof(*inode));
}

int jfs_txn_add_data(jfs_txn *tx, uint32_t ino, uint32_t index,
                     const void *data, uint32_t len)
{
    if (len > JFS_DATA_BYTES)
        return JFS_ERR_INVAL;
    return jfs_txn_add_raw(tx, JFS_OP_DATA, ino, index, data, len);
}

int jfs_txn_add_dirblk(jfs_txn *tx, uint32_t ino, uint32_t index,
                       const jfs_dirblk *blk)
{
    return jfs_txn_add_raw(tx, JFS_OP_DIRBLK, ino, index, blk, sizeof(*blk));
}

/* ------------------------------------------------------------------ */
/* commit                                                              */
/* ------------------------------------------------------------------ */

/* Split the operation list into records: every op stays whole, and each
 * record carries at most JFS_JOURNAL_REC_MAX_PAYLOAD payload bytes. */
static void chunk_range(const jfs_txn *tx, uint32_t start, uint32_t *end, size_t *bytes)
{
    size_t acc = 0;
    uint32_t i = start;

    while (i < tx->nops) {
        size_t opsz = sizeof(jfs_redo_op_hdr) + tx->ops[i].length;
        if (i > start && acc + opsz > JFS_JOURNAL_REC_MAX_PAYLOAD)
            break;
        acc += opsz;
        i++;
    }
    *end = i;
    *bytes = acc;
}

static unsigned char *serialize_chunk(const jfs_txn *tx, uint32_t from, uint32_t to,
                                      size_t *len_out)
{
    unsigned char *buf;
    size_t off = 0;
    uint32_t i;

    *len_out = 0;
    for (i = from; i < to; i++)
        *len_out += sizeof(jfs_redo_op_hdr) + tx->ops[i].length;
    buf = (unsigned char *)malloc(*len_out ? *len_out : 1);
    if (!buf)
        return NULL;
    for (i = from; i < to; i++) {
        jfs_redo_op_hdr h;
        memset(&h, 0, sizeof(h));
        h.op = tx->ops[i].op;
        h.ino = tx->ops[i].ino;
        h.index = tx->ops[i].index;
        h.length = tx->ops[i].length;
        h.crc = tx->ops[i].length ? jfs_crc32c(tx->ops[i].payload, tx->ops[i].length) : 0u;
        memcpy(buf + off, &h, sizeof(h));
        off += sizeof(h);
        if (tx->ops[i].length) {
            memcpy(buf + off, tx->ops[i].payload, tx->ops[i].length);
            off += tx->ops[i].length;
        }
    }
    return buf;
}

int jfs_journal_sync(struct jfs_fs *fs)
{
    if (fsync(fs->fd) != 0)
        return JFS_ERR_IO;
    fs->stats.syncs++;
    return JFS_OK;
}

/*
 * Blocks the redo operations of a transaction will occupy in the log region.
 * Each operation is stored as a header plus payload, padded to whole blocks.
 */
uint32_t jfs_txn_log_blocks(const jfs_txn *tx)
{
    uint32_t i, blocks = 1;         /* room for the inode map record */
    bool touches_inodes = false;

    if (!tx)
        return 0;
    for (i = 0; i < tx->nops; i++) {
        /* A log record is a jfs_log_rec_hdr followed by the redo operation
         * header and its payload, padded up to whole blocks. */
        size_t bytes = sizeof(jfs_log_rec_hdr) + sizeof(jfs_redo_op_hdr) +
                       tx->ops[i].length;
        blocks += (uint32_t)((bytes + JFS_BLOCK_SIZE - 1u) / JFS_BLOCK_SIZE);
        if (tx->ops[i].op == JFS_OP_INODE)
            touches_inodes = true;
    }
    /* Any transaction that changes an inode republishes the inode map. */
    if (!touches_inodes)
        blocks--;                   /* the map record is not written */
    return blocks;
}

int jfs_txn_commit(jfs_txn *tx, bool *needs_apply)
{
    struct jfs_fs *fs;
    uint32_t start, end;
    size_t bytes;
    uint32_t needed_blocks = 1;   /* the COMMIT record itself */
    uint32_t seq = 0;
    int rc;

    if (!tx || !tx->active)
        return JFS_ERR_STATE;
    fs = tx->fs;
    if (needs_apply)
        *needs_apply = false;

    if (tx->nops == 0) {
        tx->committed = true;
        if (needs_apply)
            *needs_apply = true;
        return JFS_OK;
    }

    /*
     * A transaction becomes durable in the journal *before* it is applied to
     * the log region, so the log must already have room for it.  Running out
     * of room *during* the apply would leave a durable transaction only
     * partly applied -- the one state this filesystem refuses to continue
     * from.  The caller retries once after forcing a checkpoint (which
     * compacts the log and reclaims dead blocks); if the transaction still
     * does not fit, the operation fails cleanly before anything is written.
     */
    if (jfs_log_free_blocks(fs) < jfs_txn_log_blocks(tx))
        return JFS_ERR_NOSPACE;

    if (!fs->journal_enabled) {
        /* Baseline comparison mode: the same append-only storage and the same
         * checkpoints, but no write ahead log and no fsync barrier between
         * building and applying a change.  Durability depends entirely on the
         * next checkpoint, and a crash in the middle of `apply` can leave a
         * partially written log record, so this mode is only used by the
         * benchmarks -- see docs/journal_protocol.md. */
        tx->committed = true;
        fs->next_txid = tx->txid + 1;
        fs->stats.txns_committed++;
        fs->commits_done++;
        if (needs_apply)
            *needs_apply = true;
        return JFS_OK;
    }

    /* Plan the OP records so the journal can be checked for room *before*
     * anything is written: a transaction must fit entirely or not at all. */
    start = 0;
    while (start < tx->nops) {
        chunk_range(tx, start, &end, &bytes);
        if (end == start)
            return JFS_ERR_INVAL;
        needed_blocks += (uint32_t)((sizeof(jfs_journal_rec_hdr) + bytes +
                                     JFS_BLOCK_SIZE - 1u) / JFS_BLOCK_SIZE);
        start = end;
    }
    if (!jfs_journal_room(fs, needed_blocks))
        return JFS_ERR_JFULL;

    {
        char why[256];
        int vrc = jfs_txn_validate(tx, why, sizeof(why));
        if (vrc != JFS_OK) {
            fprintf(stderr, "jfs: refusing to commit: %s\n", why);
            return vrc;
        }
    }

    jfs_crash_check_fs(fs, JFS_CRASH_PRE_WRITE, "before journal records");

    /* ---- phase 1: OP records, then a sync ---- */
    start = 0;
    while (start < tx->nops) {
        jfs_journal_rec_hdr h;
        unsigned char *payload;

        chunk_range(tx, start, &end, &bytes);
        payload = serialize_chunk(tx, start, end, &bytes);
        if (!payload)
            return JFS_ERR_NOMEM;

        memset(&h, 0, sizeof(h));
        h.magic       = JFS_JREC_MAGIC;
        h.version     = JFS_FORMAT_VERSION;
        h.type        = JFS_JREC_OP;
        h.txid        = tx->txid;
        h.lsn         = fs->journal_lsn + 1;
        h.seq         = seq++;
        h.nblocks     = (uint32_t)((sizeof(h) + bytes + JFS_BLOCK_SIZE - 1u) / JFS_BLOCK_SIZE);
        h.payload_len = (uint32_t)bytes;
        h.payload_crc = bytes ? jfs_crc32c(payload, bytes) : 0u;
        h.image_id    = fs->sb.image_id;
        h.ops         = end - start;
        jfs_journal_rec_finalize(&h);

        rc = jfs_journal_append(fs, &h, payload, bytes);
        free(payload);
        if (rc != JFS_OK)
            return rc;
        start = end;
    }

    jfs_crash_check_fs(fs, JFS_CRASH_POST_WRITE, "journal records written, not synced");

    bool do_sync = (fs->group_commit_batch <= 1);
    if (!do_sync && fs->group_commit_pending + 1 >= fs->group_commit_batch)
        do_sync = true;

    if (do_sync) {
        rc = jfs_journal_sync(fs);
        if (rc != JFS_OK)
            return rc;
    }

    jfs_crash_check_fs(fs, JFS_CRASH_POST_SYNC, "journal records synced, no commit record");

    /* ---- phase 2: the COMMIT record is the durability barrier ---- */
    {
        jfs_journal_rec_hdr h;

        memset(&h, 0, sizeof(h));
        h.magic       = JFS_JREC_MAGIC;
        h.version     = JFS_FORMAT_VERSION;
        h.type        = JFS_JREC_COMMIT;
        h.txid        = tx->txid;
        h.lsn         = fs->journal_lsn + 1;
        h.seq         = seq++;
        h.nblocks     = 1;
        h.payload_len = 0;
        h.payload_crc = 0;
        h.image_id    = fs->sb.image_id;
        h.ops         = 0;
        jfs_journal_rec_finalize(&h);

        rc = jfs_journal_append(fs, &h, NULL, 0);
        if (rc != JFS_OK)
            return rc;
    }

    jfs_crash_check_fs(fs, JFS_CRASH_POST_COMMIT, "commit record written, not synced");

    if (do_sync) {
        rc = jfs_journal_sync(fs);
        if (rc != JFS_OK)
            return rc;
        if (fs->group_commit_batch > 1) {
            fs->group_commit_pending = 0;
            fs->stats.group_commit_flushes++;
        }
    } else {
        fs->group_commit_pending++;
    }

    tx->committed = true;
    fs->next_txid = tx->txid + 1;
    fs->stats.txns_committed++;
    fs->commits_done++;
    if (fs->group_commit_batch > 1)
        fs->stats.group_commits++;

    jfs_crash_check_fs(fs, JFS_CRASH_POST_COMMIT_SYNC, "durable commit, changes not applied");

    if (needs_apply)
        *needs_apply = true;
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* apply                                                               */
/* ------------------------------------------------------------------ */

static const jfs_redo_op *find_block_op(const jfs_txn *tx, uint32_t ino, uint32_t index,
                                        uint16_t want_type);

/*
 * Build the inode version that a transaction describes: the payload carries
 * the already known fields, and every block index listed in provided_mask is
 * filled in from the matching data/directory operation.  During validation
 * the log address of those operations is not assigned yet, so a placeholder
 * is used; the length is what the consistency rules depend on.
 */
static int patch_inode(const jfs_txn *tx, const jfs_redo_op *o, jfs_inode *out)
{
    jfs_inode inode;
    uint32_t b;

    if (o->length != sizeof(jfs_inode))
        return JFS_ERR_CORRUPT;
    memcpy(&inode, o->payload, sizeof(inode));

    for (b = 0; b < JFS_MAX_FILE_BLOCKS; b++) {
        const jfs_redo_op *dop;

        if (!(inode.provided_mask & (1u << b)))
            continue;
        dop = find_block_op(tx, inode.ino, b, JFS_OP_DATA);
        if (!dop)
            dop = find_block_op(tx, inode.ino, b, JFS_OP_DIRBLK);
        if (!dop) {
            if (inode.type == JFS_TYPE_DIR) {
                inode.addrs[b] = 0;
                inode.blklen[b] = 0;
                continue;
            }
            /* A transaction claims to provide a block but carries no content
             * for it: the journal is inconsistent. */
            return JFS_ERR_CORRUPT;
        }
        inode.addrs[b] = dop->addr ? dop->addr : 1u;
        inode.blklen[b] = dop->length;
    }
    *out = inode;
    return JFS_OK;
}

/*
 * Validate a transaction before anything is written to the journal.  A
 * transaction that cannot be applied must not become durable: otherwise the
 * journal would hold a committed record that recovery would fail on for ever
 * (see docs/journal_protocol.md, "validate before commit").
 */
int jfs_txn_validate(const jfs_txn *tx, char *err, size_t errsz)
{
    uint32_t i;

    for (i = 0; i < tx->nops; i++) {
        const jfs_redo_op *o = &tx->ops[i];

        if (o->ino == 0 || o->ino > JFS_MAX_INODES) {
            snprintf(err, errsz, "operation %u has an invalid inode number", i);
            return JFS_ERR_CORRUPT;
        }
        switch (o->op) {
        case JFS_OP_INODE: {
            jfs_inode inode;
            int rc = patch_inode(tx, o, &inode);

            if (rc != JFS_OK) {
                snprintf(err, errsz,
                         "inode %u: a provided block has no content in this transaction",
                         o->ino);
                return rc;
            }
            inode.self = 1;                  /* placeholder for validation */
            if (!(inode.flags & JFS_INODE_FREE) && !jfs_inode_sane(&inode)) {
                snprintf(err, errsz,
                         "inode %u would not be self consistent (size %llu, "
                         "blocks %u)", inode.ino, (unsigned long long)inode.size,
                         inode.nblocks);
                return JFS_ERR_CORRUPT;
            }
            break;
        }
        case JFS_OP_DATA:
            if (o->length == 0 || o->length > JFS_DATA_BYTES ||
                o->index >= JFS_MAX_FILE_BLOCKS) {
                snprintf(err, errsz, "data operation %u is malformed", i);
                return JFS_ERR_CORRUPT;
            }
            break;
        case JFS_OP_DIRBLK:
            if (o->length != sizeof(jfs_dirblk) || o->index >= JFS_MAX_FILE_BLOCKS) {
                snprintf(err, errsz, "directory operation %u is malformed", i);
                return JFS_ERR_CORRUPT;
            }
            break;
        default:
            snprintf(err, errsz, "operation %u has an unknown type", i);
            return JFS_ERR_CORRUPT;
        }
    }
    return JFS_OK;
}

static const jfs_redo_op *find_block_op(const jfs_txn *tx, uint32_t ino, uint32_t index,
                                        uint16_t want_type)
{
    uint32_t i;

    for (i = 0; i < tx->nops; i++) {
        const jfs_redo_op *o = &tx->ops[i];
        if (o->op == want_type && o->ino == ino && o->index == index)
            return o;
    }
    return NULL;
}

/* Append a fresh imap record describing the current in-memory map. */
static int imap_publish(struct jfs_fs *fs)
{
    jfs_imap copy = fs->imap;
    uint64_t addr = 0;
    int rc;

    copy.crc = 0;
    copy.crc = jfs_crc_struct(&copy, sizeof(copy), offsetof(jfs_imap, crc));
    rc = jfs_log_append(fs, JFS_LREC_IMAP, 0, 0, &copy, sizeof(copy), &addr);
    if (rc != JFS_OK)
        return rc;
    fs->imap = copy;
    fs->ckpt.imap_addr = addr;
    return JFS_OK;
}

int jfs_txn_apply(jfs_txn *tx)
{
    struct jfs_fs *fs;
    uint32_t i;
    bool imap_dirty = false;
    int rc = JFS_OK;

    if (!tx || !tx->committed)
        return JFS_ERR_STATE;
    fs = tx->fs;

    /* ---- pass A: copy block contents into the log ---- */
    for (i = 0; i < tx->nops; i++) {
        jfs_redo_op *o = &tx->ops[i];

        if (o->op == JFS_OP_DATA) {
            rc = jfs_log_append(fs, JFS_LREC_DATA, o->ino, o->index,
                                o->payload, o->length, &o->addr);
        } else if (o->op == JFS_OP_DIRBLK) {
            rc = jfs_log_append(fs, JFS_LREC_DIRBLK, o->ino, o->index,
                                o->payload, o->length, &o->addr);
        } else if (o->op != JFS_OP_INODE) {
            return JFS_ERR_CORRUPT;
        }
        if (rc != JFS_OK)
            return rc;
    }

    jfs_crash_check_fs(fs, JFS_CRASH_AFTER_IMAP, "block records appended");

    /* ---- pass B: write the new inode versions ---- */
    for (i = 0; i < tx->nops; i++) {
        jfs_redo_op *o = &tx->ops[i];
        jfs_inode inode;
        jfs_imap_entry *e;

        if (o->op != JFS_OP_INODE)
            continue;
        {
            int prc = patch_inode(tx, o, &inode);
            if (prc != JFS_OK)
                return prc;
        }

        e = jfs_imap_lookup(fs, inode.ino);
        if (!e)
            return JFS_ERR_CORRUPT;

        if (inode.flags & JFS_INODE_FREE) {
            /* Deletion: the inode leaves the map and every record it
             * referenced becomes unreachable (reclaimed at compaction). */
            if (e->state == JFS_IMAP_USED && fs->imap.count > 0)
                fs->imap.count--;
            e->state = JFS_IMAP_FREE;
            e->addr = 0;
            imap_dirty = true;
            continue;
        }

        /* provided_mask is a per-transaction field: it describes which block
         * references this operation carries.  Persist it cleared so a later
         * metadata-only update (chmod, for example) does not look as if it
         * provided blocks it never wrote. */
        inode.provided_mask = 0;
        inode.self = ((uint64_t)fs->ckpt.log_half << 32) | fs->ckpt.log_head;
        rc = jfs_log_append(fs, JFS_LREC_INODE, inode.ino, 0,
                            &inode, sizeof(inode), &o->addr);
        if (rc != JFS_OK)
            return rc;

        if (e->state != JFS_IMAP_USED) {
            e->state = JFS_IMAP_USED;
            e->ino = inode.ino;
            fs->imap.count++;
        }
        e->addr = o->addr;
        if (inode.ino >= fs->imap.next_ino)
            fs->imap.next_ino = inode.ino + 1;
        imap_dirty = true;
    }

    if (imap_dirty) {
        rc = imap_publish(fs);
        if (rc != JFS_OK)
            return rc;
    }

    fs->stats.redo_ops += tx->nops;

    jfs_crash_check_fs(fs, JFS_CRASH_POST_APPLY, "log updated, checkpoint pending");
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* end: make the applied state durable                                 */
/* ------------------------------------------------------------------ */

int jfs_finish_transaction(struct jfs_fs *fs, uint64_t applied_txid)
{
    uint64_t nstart = fs->ckpt.journal_start;
    uint64_t nused = fs->ckpt.journal_used;
    uint64_t freed = 0;
    jfs_checkpoint nc;
    int rc;

    if (!fs || !fs->mounted)
        return JFS_ERR_NOTMOUNTED;

    /* 1. plan the window that becomes unnecessary.  The plan is applied to
     *    the in-memory state only after the checkpoint naming it is on disk,
     *    so a crash cannot leave the blocks reusable while the durable
     *    checkpoint still needs the records they hold. */
    rc = jfs_journal_window_plan(fs, applied_txid, &nstart, &nused, &freed);
    if (rc != JFS_OK)
        return rc;

    nc = fs->ckpt;
    nc.seq          = fs->ckpt.seq + 1;
    nc.applied_txid = applied_txid;
    nc.journal_start = nstart;
    nc.journal_used  = (uint32_t)nused;
    nc.log_seq       = fs->log_seq;
    nc.imap_addr     = fs->ckpt.imap_addr;
    nc.next_txid     = fs->next_txid;
    nc.n_inodes      = fs->imap.count;
    nc.live_bytes    = fs->log_live_bytes;
    nc.replayed_total = fs->replayed;

    /*
     * Batching (see `set checkpoint-interval N`): with N > 1 the checkpoint
     * and the resulting journal reclamation are deferred for up to N-1
     * transactions.  Fewer checkpoints and fewer fsyncs make updates faster,
     * and the price is paid at recovery time, which then has to redo every
     * transaction after the previous checkpoint.  The window must therefore
     * keep those records and must still have room for them: the guard below
     * keeps at least 8 free journal blocks at all times, and the deferral
     * lasts at most N-1 transactions, after which the checkpoint is forced.
     */
    if (fs->checkpoint_interval > 1 &&
        fs->txns_since_ckpt + 1 < fs->checkpoint_interval &&
        fs->ckpt.journal_used + 8u < fs->sb.journal_blocks) {
        fs->last_applied = applied_txid;
        fs->txns_since_ckpt++;
        return JFS_OK;
    }

    if (fs->group_commit_pending > 0) {
        rc = jfs_journal_sync(fs);
        if (rc != JFS_OK)
            return rc;
        fs->group_commit_pending = 0;
        fs->stats.group_commit_flushes++;
    }

    jfs_crash_check_fs(fs, JFS_CRASH_CHECKPOINT_MID, "checkpoint update starting");

    {
        int slot = (fs->ckpt_slot + 1) % 2;
        rc = jfs_checkpoint_write_slot(fs, &nc, slot, false);
        if (rc != JFS_OK)
            return rc;

        jfs_crash_check_fs(fs, JFS_CRASH_CHECKPOINT_POST,
                           "checkpoint block written, not yet synced");

        rc = jfs_journal_sync(fs);
        if (rc != JFS_OK)
            return rc;
        fs->ckpt_slot = slot;
        fs->stats.checkpoint_seq = nc.seq;
    }

    /* 2. the checkpoint is durable: publish the new window */
    fs->ckpt = nc;
    fs->last_applied = applied_txid;
    fs->txns_since_ckpt = 0;
    fs->stats.journal_blocks_freed += freed;

    /* 3. keep the active log half from filling up completely */
    if (jfs_log_free_blocks(fs) < jfs_log_half_blocks(fs) / 4u) {
        rc = jfs_log_compact(fs);
        if (rc != JFS_OK)
            return rc;
    }
    return JFS_OK;
}

int jfs_txn_end(jfs_txn *tx)
{
    int rc;

    if (!tx)
        return JFS_ERR_INVAL;
    rc = jfs_finish_transaction(tx->fs, tx->txid);
    jfs_txn_free(tx);
    return rc;
}

uint64_t jfs_journal_replay_count(const struct jfs_fs *fs)
{
    return fs ? fs->replayed : 0u;
}
