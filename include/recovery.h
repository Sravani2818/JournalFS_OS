/*
 * recovery.h -- Layer 4, the recovery manager.
 *
 * Recovery runs automatically inside jfs_mount().  It performs a single
 * forward pass over the journal window:
 *
 *   - records that fail their magic/version/CRC checks are reported and
 *     treated as the end of the valid window (a torn tail);
 *   - a transaction is COMMITTED iff its COMMIT record is present and valid;
 *   - committed transactions whose txid is greater than the checkpoint's
 *     `applied_txid` are replayed (redo only -- the log region is
 *     append-only, so applying the same operation twice is harmless because
 *     the newest version of a record wins);
 *   - incomplete transactions are ignored: they were never durable, so the
 *     old state on disk is the correct state;
 *   - the journal window is advanced past the replayed transactions.
 *
 * The pass is idempotent: running recovery twice produces the same filesystem
 * state and the second pass has nothing left to do.
 */
#ifndef JFS_RECOVERY_H
#define JFS_RECOVERY_H

#include <stdint.h>
#include <stdbool.h>

#include "disk_format.h"

struct jfs_fs;

typedef struct {
    uint64_t txns_scanned;
    uint64_t txns_committed;      /* committed but not yet applied           */
    uint64_t txns_incomplete;     /* no commit record: ignored               */
    uint64_t txns_skipped;        /* already applied before the crash        */
    uint64_t records_valid;
    uint64_t records_invalid;
    uint64_t redo_ops_replayed;
    uint64_t bytes_replayed;
    uint64_t journal_blocks_before;
    uint64_t journal_blocks_after;
    uint64_t duration_us;
} jfs_recovery_stats;

/* Run recovery on a freshly opened image.  Must be called after the
 * checkpoint and superblock were validated and before the filesystem is
 * used.  Fills `stats` when it is not NULL. */
int jfs_recover(struct jfs_fs *fs, jfs_recovery_stats *stats);

/* Verify the in-memory structures against what is reachable on disk.
 * Returns JFS_OK when the filesystem is internally consistent. */
int jfs_check_consistency(struct jfs_fs *fs, char *err, size_t errsz);

/* Statistics of the most recent jfs_recover() call (NULL before the first). */
const jfs_recovery_stats *jfs_last_recovery_stats(void);

/* Dump the journal window: every record plus the transaction groups and
 * whether recovery would consider them committed. */
void jfs_journal_dump(struct jfs_fs *fs, void (*emit)(void *, const char *), void *ud);

/* Human readable summary for the CLI. */
void jfs_recovery_report(const jfs_recovery_stats *st, void (*emit)(void *, const char *), void *ud);

#endif /* JFS_RECOVERY_H */
