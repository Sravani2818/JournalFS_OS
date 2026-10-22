/*
 * recovery.c -- Layer 4, the recovery manager.
 *
 * A single forward scan of the journal window decides everything:
 *
 *   - a record whose header or payload fails validation ends the scan
 *     ("torn tail"); whatever follows is not trusted;
 *   - contiguous records with the same transaction id form a group; a group
 *     counts as committed only when it contains a valid COMMIT record;
 *   - committed groups newer than the checkpoint's applied_txid are redone;
 *   - committed groups that are already applied are skipped;
 *   - groups without a COMMIT record are never applied.  They were never
 *     reported as durable, so the previous state is the correct state and
 *     there is nothing to roll back thanks to the append-only storage.
 *
 * Redo is idempotent: jfs_txn_apply() appends new versions of records
 * instead of patching old ones, so applying a transaction twice (or replaying
 * after a crash in the middle of recovery) always converges to the same
 * logical filesystem state.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "checksum.h"
#include "directory.h"
#include "crash.h"
#include "inode.h"
#include "journal.h"
#include "journalfs.h"
#include "recovery.h"
#include "storage.h"
#include "util.h"

typedef struct {
    uint32_t win_index;      /* first block of the record inside the window  */
    uint32_t nblocks;
    uint64_t txid;
    uint64_t lsn;
    uint16_t type;
    uint32_t payload_len;
    uint32_t ops;
} jrec;

typedef struct {
    uint32_t first_record;
    uint32_t nrecords;
    uint64_t txid;
    bool     committed;
} jgroup;

static const jfs_recovery_stats *g_last;

static uint64_t now_us(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
        return 0;
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */
/* ------------------------------------------------------------------ */

static int scan_window(struct jfs_fs *fs, jrec *recs, uint32_t max_recs,
                       uint32_t *nrecs_out, jfs_recovery_stats *st)
{
    uint32_t idx = 0;
    uint32_t n = 0;

    while (idx < fs->ckpt.journal_used && n < max_recs) {
        jfs_journal_rec_hdr h;
        int rc = jfs_journal_read_hdr(fs, idx, &h);

        if (rc != JFS_OK) {
            /* Either the honest end of the window or a damaged record: both
             * mean "stop trusting the journal from here". */
            st->records_invalid++;
            break;
        }
        recs[n].win_index   = idx;
        recs[n].nblocks     = h.nblocks;
        recs[n].txid        = h.txid;
        recs[n].lsn         = h.lsn;
        recs[n].type        = h.type;
        recs[n].payload_len = h.payload_len;
        recs[n].ops         = h.ops;
        st->records_valid++;
        n++;
        idx += h.nblocks;
    }
    *nrecs_out = n;
    return JFS_OK;
}

static void group_records(jrec *recs, uint32_t nrecs, jgroup *groups,
                          uint32_t *ngroups_out, jfs_recovery_stats *st)
{
    uint32_t i = 0;
    uint32_t g = 0;

    while (i < nrecs) {
        jgroup grp;
        uint32_t j;

        grp.first_record = i;
        grp.txid = recs[i].txid;
        grp.committed = false;
        for (j = i; j < nrecs; j++) {
            if (recs[j].txid != grp.txid)
                break;
            if (recs[j].type == JFS_JREC_COMMIT)
                grp.committed = true;
            if (j > i && recs[j - 1].type == JFS_JREC_COMMIT)
                break;              /* a second transaction reused the txid */
        }
        grp.nrecords = j - i;
        groups[g++] = grp;
        st->txns_scanned++;
        if (!grp.committed)
            st->txns_incomplete++;
        i = j;
    }
    *ngroups_out = g;
}

/* ------------------------------------------------------------------ */
/* rebuilding transactions                                             */
/* ------------------------------------------------------------------ */

static int ops_from_payload(jfs_txn *tx, const unsigned char *buf, size_t len,
                            uint32_t *ops_out)
{
    size_t off = 0;
    uint32_t ops = 0;

    while (off < len) {
        jfs_redo_op_hdr h;

        if (off + sizeof(h) > len)
            return JFS_ERR_CORRUPT;
        memcpy(&h, buf + off, sizeof(h));
        off += sizeof(h);
        if (h.length > len - off)
            return JFS_ERR_CORRUPT;
        if (h.length && jfs_crc32c(buf + off, h.length) != h.crc)
            return JFS_ERR_CORRUPT;

        switch (h.op) {
        case JFS_OP_INODE:
            if (h.length != sizeof(jfs_inode))
                return JFS_ERR_CORRUPT;
            break;
        case JFS_OP_DATA:
            if (h.length == 0 || h.length > JFS_DATA_BYTES)
                return JFS_ERR_CORRUPT;
            if (h.index >= JFS_MAX_FILE_BLOCKS)
                return JFS_ERR_CORRUPT;
            break;
        case JFS_OP_DIRBLK:
            if (h.length != sizeof(jfs_dirblk))
                return JFS_ERR_CORRUPT;
            if (h.index >= JFS_MAX_FILE_BLOCKS)
                return JFS_ERR_CORRUPT;
            break;
        default:
            return JFS_ERR_CORRUPT;   /* unknown redo operation */
        }
        {
            int rc = jfs_txn_add_raw(tx, h.op, h.ino, h.index, buf + off, h.length);
            if (rc != JFS_OK)
                return rc;
        }
        off += h.length;
        ops++;
    }
    *ops_out = ops;
    return JFS_OK;
}

/* Rebuild one committed transaction from its journal records. */
static int rebuild_txn(struct jfs_fs *fs, const jrec *recs, const jgroup *grp,
                       jfs_txn **out, jfs_recovery_stats *st)
{
    jfs_txn *tx;
    uint32_t i;
    int rc = JFS_OK;

    tx = (jfs_txn *)calloc(1, sizeof(*tx));
    if (!tx)
        return JFS_ERR_NOMEM;
    tx->fs = fs;
    tx->txid = grp->txid;
    tx->active = true;

    for (i = grp->first_record; i < grp->first_record + grp->nrecords; i++) {
        unsigned char *payload;
        jfs_journal_rec_hdr h;
        uint32_t ops = 0;

        if (recs[i].type != JFS_JREC_OP)
            continue;
        rc = jfs_journal_read_hdr(fs, recs[i].win_index, &h);
        if (rc != JFS_OK)
            break;
        payload = (unsigned char *)malloc(h.payload_len ? h.payload_len : 1);
        if (!payload) {
            rc = JFS_ERR_NOMEM;
            break;
        }
        rc = jfs_journal_read_payload(fs, recs[i].win_index, &h, payload, h.payload_len);
        if (rc == JFS_OK)
            rc = ops_from_payload(tx, payload, h.payload_len, &ops);
        free(payload);
        if (rc != JFS_OK)
            break;
        st->redo_ops_replayed += ops;
        st->bytes_replayed += h.payload_len;
    }

    if (rc == JFS_OK && tx->nops == 0) {
        /* A committed transaction without operations cannot happen with this
         * implementation; treat it as damage rather than silently skipping. */
        rc = JFS_ERR_CORRUPT;
    }
    if (rc != JFS_OK) {
        jfs_txn_free(tx);
        return rc;
    }
    tx->committed = true;   /* the COMMIT record is present and valid */
    {
        char why[256];
        int vrc = jfs_txn_validate(tx, why, sizeof(why));
        if (vrc != JFS_OK) {
            fprintf(stderr,
                    "jfs: journal transaction %llu cannot be replayed: %s\n",
                    (unsigned long long)tx->txid, why);
            jfs_txn_free(tx);
            return vrc;
        }
    }
    *out = tx;
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* recovery                                                            */
/* ------------------------------------------------------------------ */

int jfs_recover(struct jfs_fs *fs, jfs_recovery_stats *stats)
{
    static jfs_recovery_stats st;
    jrec *recs = NULL;
    jgroup *groups = NULL;
    uint32_t nrecs = 0, ngroups = 0, g;
    uint64_t applied_max = fs->ckpt.applied_txid;
    uint64_t max_txid = fs->ckpt.applied_txid;
    uint64_t t0 = now_us();
    int rc = JFS_OK;
    bool replayed_any = false;

    memset(&st, 0, sizeof(st));
    st.journal_blocks_before = fs->ckpt.journal_used;

    recs = (jrec *)calloc(fs->sb.journal_blocks ? fs->sb.journal_blocks : 1, sizeof(*recs));
    groups = (jgroup *)calloc(fs->sb.journal_blocks ? fs->sb.journal_blocks : 1, sizeof(*groups));
    if (!recs || !groups) {
        rc = JFS_ERR_NOMEM;
        goto out;
    }

    rc = scan_window(fs, recs, fs->sb.journal_blocks, &nrecs, &st);
    if (rc != JFS_OK)
        goto out;
    group_records(recs, nrecs, groups, &ngroups, &st);

    /* Learn the highest transaction id present so that ids never repeat. */
    for (g = 0; g < ngroups; g++) {
        if (groups[g].txid > max_txid)
            max_txid = groups[g].txid;
    }
    if (max_txid + 1 > fs->next_txid)
        fs->next_txid = max_txid + 1;

    for (g = 0; g < ngroups; g++) {
        jfs_txn *tx = NULL;

        if (!groups[g].committed)
            continue;                 /* incomplete: never exposed */
        if (groups[g].txid <= fs->ckpt.applied_txid) {
            st.txns_skipped++;
            continue;                 /* already applied before the crash */
        }

        jfs_crash_check_fs(fs, JFS_CRASH_RECOVER_MID, "replaying committed transactions");

        rc = rebuild_txn(fs, recs, &groups[g], &tx, &st);
        if (rc != JFS_OK)
            goto out;

        rc = jfs_txn_apply(tx);
        jfs_txn_free(tx);
        if (rc != JFS_OK)
            goto out;

        st.txns_committed++;
        replayed_any = true;
        if (groups[g].txid > applied_max)
            applied_max = groups[g].txid;
        fs->replayed++;
        fs->stats.txns_replayed++;
    }

    if (replayed_any) {
        rc = jfs_finish_transaction(fs, applied_max);
        if (rc != JFS_OK)
            goto out;
    }
    st.journal_blocks_after = fs->ckpt.journal_used;

out:
    st.duration_us = now_us() - t0;
    if (stats)
        *stats = st;
    g_last = &st;
    free(recs);
    free(groups);
    return rc;
}

const jfs_recovery_stats *jfs_last_recovery_stats(void)
{
    return g_last;
}

void jfs_recovery_report(const jfs_recovery_stats *st,
                         void (*emit)(void *, const char *), void *ud)
{
    char line[256];

    if (!st || !emit)
        return;
    snprintf(line, sizeof(line), "journal: %llu valid record(s), %llu unusable",
             (unsigned long long)st->records_valid,
             (unsigned long long)st->records_invalid);
    emit(ud, line);
    snprintf(line, sizeof(line),
             "transactions: %llu scanned, %llu committed and replayed, "
             "%llu already applied, %llu incomplete (ignored)",
             (unsigned long long)st->txns_scanned,
             (unsigned long long)st->txns_committed,
             (unsigned long long)st->txns_skipped,
             (unsigned long long)st->txns_incomplete);
    emit(ud, line);
    snprintf(line, sizeof(line), "redo: %llu operation(s), %llu payload bytes replayed",
             (unsigned long long)st->redo_ops_replayed,
             (unsigned long long)st->bytes_replayed);
    emit(ud, line);
    snprintf(line, sizeof(line), "journal window: %llu -> %llu block(s), recovery took %llu us",
             (unsigned long long)st->journal_blocks_before,
             (unsigned long long)st->journal_blocks_after,
             (unsigned long long)st->duration_us);
    emit(ud, line);
}

/* ------------------------------------------------------------------ */
/* consistency check                                                   */
/* ------------------------------------------------------------------ */

int jfs_check_consistency(struct jfs_fs *fs, char *err, size_t errsz)
{
    uint32_t used = 0;
    uint32_t i;
    char line[256];

    for (i = 0; i < JFS_MAX_INODES; i++) {
        const jfs_imap_entry *e = &fs->imap.entries[i];
        jfs_inode inode;
        uint32_t b;

        if (e->state != JFS_IMAP_USED)
            continue;
        used++;
        if (jfs_log_read_inode(fs, e->addr, &inode) != JFS_OK) {
            snprintf(err, errsz, "inode %u: record at 0x%llx is unreadable",
                     e->ino, (unsigned long long)e->addr);
            return JFS_ERR_CORRUPT;
        }
        if (inode.ino != e->ino) {
            snprintf(err, errsz, "inode map entry %u points at inode %u",
                     e->ino, inode.ino);
            return JFS_ERR_CORRUPT;
        }
        for (b = 0; b < inode.nblocks; b++) {
            unsigned char tmp[JFS_DATA_BYTES];
            uint16_t type = 0;
            uint32_t len = 0;

            if (inode.addrs[b] == 0) {
                snprintf(err, errsz, "inode %u: block %u has no address",
                         inode.ino, b);
                return JFS_ERR_CORRUPT;
            }
            if (jfs_log_read(fs, inode.addrs[b], tmp, sizeof(tmp), &type, &len) != JFS_OK) {
                snprintf(err, errsz, "inode %u: data block %u is unreadable",
                         inode.ino, b);
                return JFS_ERR_CORRUPT;
            }
            if (inode.type == JFS_TYPE_DIR && type != JFS_LREC_DIRBLK) {
                snprintf(err, errsz, "inode %u: block %u is not a directory block",
                         inode.ino, b);
                return JFS_ERR_CORRUPT;
            }
            if (inode.type == JFS_TYPE_FILE && type != JFS_LREC_DATA) {
                snprintf(err, errsz, "inode %u: block %u is not a file data record",
                         inode.ino, b);
                return JFS_ERR_CORRUPT;
            }
        }
        if (inode.type == JFS_TYPE_DIR) {
            jfs_dirview dv;
            uint32_t k, m;

            if (jfs_dirview_open(fs, inode.ino, &dv) != JFS_OK) {
                snprintf(err, errsz, "inode %u: directory blocks are damaged",
                         inode.ino);
                return JFS_ERR_CORRUPT;
            }
            for (k = 0; k < dv.nblocks; k++) {
                for (m = 0; m < dv.blk[k].count; m++) {
                    const jfs_dirent *de = &dv.blk[k].ents[m];
                    jfs_imap_entry *te = jfs_imap_lookup(fs, de->ino);
                    if (te == NULL || te->state != JFS_IMAP_USED) {
                        snprintf(err, errsz,
                                 "directory %u: entry '%s' points at missing inode %u",
                                 inode.ino, de->name, de->ino);
                        jfs_dirview_free(&dv);
                        return JFS_ERR_CORRUPT;
                    }
                }
            }
            jfs_dirview_free(&dv);
        }
    }

    if (used != fs->imap.count) {
        snprintf(err, errsz, "inode map claims %u used inodes but %u are reachable",
                 fs->imap.count, used);
        return JFS_ERR_CORRUPT;
    }
    if (fs->imap.count != fs->ckpt.n_inodes) {
        snprintf(err, errsz, "checkpoint records %u inodes, the map has %u",
                 fs->ckpt.n_inodes, fs->imap.count);
        return JFS_ERR_CORRUPT;
    }
    snprintf(line, sizeof(line),
             "filesystem is consistent: %u inode(s), %llu live bytes in the log",
             used, (unsigned long long)fs->log_live_bytes);
    if (err && errsz) {
        if (fs->journal_truncated)
            /* The tree and the log region agree with each other, but the
             * journal was cut short: say so instead of a bare "consistent",
             * because transactions written after the damage are gone. */
            snprintf(err, errsz,
                     "%s; warning: the journal window is truncated at block "
                     "%llu (damaged record), later transactions were not "
                     "replayed", line,
                     (unsigned long long)fs->journal_damaged_block);
        else
            snprintf(err, errsz, "%s", line);
    }
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* journal inspection (used by the `journal` command)                  */
/* ------------------------------------------------------------------ */

void jfs_journal_dump(struct jfs_fs *fs, void (*emit)(void *, const char *), void *ud)
{
    jrec *recs;
    jgroup *groups;
    jfs_recovery_stats st;
    uint32_t nrecs = 0, ngroups = 0, g;
    char line[256];

    memset(&st, 0, sizeof(st));
    recs = (jrec *)calloc(fs->sb.journal_blocks ? fs->sb.journal_blocks : 1, sizeof(*recs));
    groups = (jgroup *)calloc(fs->sb.journal_blocks ? fs->sb.journal_blocks : 1, sizeof(*groups));
    if (!recs || !groups) {
        emit(ud, "out of memory");
        free(recs);
        free(groups);
        return;
    }
    scan_window(fs, recs, fs->sb.journal_blocks, &nrecs, &st);
    group_records(recs, nrecs, groups, &ngroups, &st);

    snprintf(line, sizeof(line),
             "journal window: start=%llu blocks, used=%llu of %u, applied_txid=%llu%s",
             (unsigned long long)fs->ckpt.journal_start,
             (unsigned long long)fs->ckpt.journal_used,
             fs->sb.journal_blocks,
             (unsigned long long)fs->ckpt.applied_txid,
             fs->journal_truncated ? "  ** DAMAGED **" : "");
    emit(ud, line);
    if (fs->journal_truncated) {
        snprintf(line, sizeof(line),
                 "  the window is truncated at block %llu: that record cannot be "
                 "read, so transactions after it cannot be replayed",
                 (unsigned long long)fs->journal_damaged_block);
        emit(ud, line);
    }

    for (uint32_t i = 0; i < nrecs; i++) {
        snprintf(line, sizeof(line),
                 "  rec[%2u] win-block %-3u txid=%-4llu lsn=%-4llu %-6s blocks=%-2u payload=%-6u ops=%u",
                 i, recs[i].win_index, (unsigned long long)recs[i].txid,
                 (unsigned long long)recs[i].lsn,
                 recs[i].type == JFS_JREC_OP ? "OP" :
                 recs[i].type == JFS_JREC_COMMIT ? "COMMIT" :
                 recs[i].type == JFS_JREC_ABORT ? "ABORT" : "OTHER",
                 recs[i].nblocks, recs[i].payload_len, recs[i].ops);
        emit(ud, line);
    }
    for (g = 0; g < ngroups; g++) {
        snprintf(line, sizeof(line), "  tx %llu: %u record(s), %s",
                 (unsigned long long)groups[g].txid, groups[g].nrecords,
                 groups[g].committed ? "COMMITTED" : "incomplete (ignored by recovery)");
        emit(ud, line);
    }
    free(recs);
    free(groups);
}
