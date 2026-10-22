/*
 * journal.h -- transaction manager interface (write ahead logging).
 *
 * Protocol implemented by this module (see docs/journal_protocol.md):
 *
 *   1. jfs_txn_begin()      allocate a transaction id and a redo buffer.
 *   2. jfs_txn_add_*()      describe the intended changes in memory only.
 *   3. jfs_txn_commit()     append OP record(s), fsync the journal, append
 *                           the COMMIT record, fsync again.  Only after the
 *                           second fsync may the changes be applied.
 *   4. jfs_txn_apply()      replay the operations into the log-structured
 *                           log region and update the checkpoint.
 *   5. jfs_journal_checkpoint_advance() drop journal records that are no
 *                           longer needed for recovery.
 *
 * A transaction that was interrupted before step 3 completed has no COMMIT
 * record on disk and is therefore invisible to recovery: the "old" state is
 * the only state that exists.  A transaction interrupted between step 3 and
 * step 4 is replayed by the recovery manager at mount time.
 */
#ifndef JFS_JOURNAL_H
#define JFS_JOURNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "disk_format.h"

struct jfs_fs;

/* ---------------- checksums on records ---------------- */

void jfs_log_rec_finalize(jfs_log_rec_hdr *h);
bool jfs_log_rec_check(const jfs_log_rec_hdr *h);
void jfs_journal_rec_finalize(jfs_journal_rec_hdr *h);
bool jfs_journal_rec_check(const jfs_journal_rec_hdr *h);

/* ---------------- raw journal region I/O ---------------- */

/* Map a journal-relative block index to an absolute image byte offset. */
uint64_t jfs_journal_block_off(const struct jfs_fs *fs, uint64_t index);

/* Append one journal record at the current journal head and sync it.
 * `hdr` must already be finalised (header CRC computed). */
int jfs_journal_append(struct jfs_fs *fs, const jfs_journal_rec_hdr *hdr,
                       const void *payload, size_t payload_len);

/* Remove every journal record of transactions with txid <= upto, reclaiming
 * journal space.  Returns the number of blocks freed. */
int jfs_journal_checkpoint_advance(struct jfs_fs *fs, uint64_t upto, uint64_t *freed_blocks);

/* Would `blocks` more journal blocks fit in the live window? */
bool jfs_journal_room(const struct jfs_fs *fs, uint32_t blocks);

/* Compute the journal window that would remain after freeing every leading
 * transaction with txid <= upto.  Does not modify the filesystem: the caller
 * applies the result only after the checkpoint naming it is durable. */
int jfs_journal_window_plan(struct jfs_fs *fs, uint64_t upto,
                            uint64_t *start, uint64_t *used, uint64_t *freed);

/* Rebuild the in-memory journal window bookkeeping by scanning the region. */
int jfs_journal_rescan(struct jfs_fs *fs);

/* Reset the journal window (used by format and by compaction). */
void jfs_journal_reset(struct jfs_fs *fs);

/* Read one record header / payload at a window-relative block index,
 * validating magic, version, image id, CRCs and the claimed block count. */
int jfs_journal_read_hdr(struct jfs_fs *fs, uint64_t win_index,
                         jfs_journal_rec_hdr *out);
int jfs_journal_read_payload(struct jfs_fs *fs, uint64_t win_index,
                             const jfs_journal_rec_hdr *hdr, void *buf, size_t bufsz);

/* Sync the journal (and, on a real device, everything written so far). */
int jfs_journal_sync(struct jfs_fs *fs);

/* ---------------- transactions ---------------- */

typedef struct {
    uint16_t op;
    uint32_t ino;
    uint32_t index;
    uint32_t length;             /* payload length in bytes                */
    unsigned char *payload;      /* malloc'd copy                          */
    uint64_t addr;               /* log address assigned when applied      */
} jfs_redo_op;

typedef struct jfs_txn {
    struct jfs_fs *fs;
    uint64_t txid;
    uint32_t nops;
    uint32_t cap;
    jfs_redo_op *ops;
    size_t payload_bytes;
    bool active;
    bool committed;
} jfs_txn;

/* Create a transaction handle (no disk writes yet). */
int  jfs_txn_begin(struct jfs_fs *fs, jfs_txn **out);
/* Discard a transaction without writing anything. */
void jfs_txn_abort(jfs_txn *tx);
/* Replay the operations, sync the journal, then write the COMMIT record and
 * sync it.  On success the transaction is durable and *needs_apply is set to
 * true (the caller then calls jfs_txn_apply + jfs_txn_end). */
int  jfs_txn_commit(jfs_txn *tx, bool *needs_apply);
/* Apply the redo operations to the log-structured log region. */
int  jfs_txn_apply(jfs_txn *tx);
/* Mark the transaction applied and free its memory. */
int  jfs_txn_end(jfs_txn *tx);
/* Free memory without applying (used after a crash recovery path). */
void jfs_txn_free(jfs_txn *tx);

/* Log-region blocks the transaction's redo operations will occupy. */
uint32_t jfs_txn_log_blocks(const jfs_txn *tx);

/* Check that a transaction can be applied (types, sizes, block references and
 * inode self consistency) before anything reaches the journal.  Returns
 * JFS_OK or an error with a human readable reason in `err`. */
int jfs_txn_validate(const jfs_txn *tx, char *err, size_t errsz);

/* Add an operation straight from a decoded journal payload (recovery path). */
int jfs_txn_add_raw(jfs_txn *tx, uint16_t op, uint32_t ino, uint32_t index,
                    const void *payload, uint32_t len);

/* Redo-operation constructors. */
int jfs_txn_add_inode(jfs_txn *tx, const jfs_inode *inode);
int jfs_txn_add_data(jfs_txn *tx, uint32_t ino, uint32_t index,
                     const void *data, uint32_t len);
int jfs_txn_add_dirblk(jfs_txn *tx, uint32_t ino, uint32_t index,
                       const jfs_dirblk *blk);


/* Number of replayed transactions since mount (recovery statistics). */
uint64_t jfs_journal_replay_count(const struct jfs_fs *fs);

#endif /* JFS_JOURNAL_H */
