/*
 * journalfs.h -- Layer 2, filesystem operations and core state.
 *
 * `struct jfs_fs` is the in-memory view of a mounted image.  Filesystem
 * operations never touch the disk directly: they describe their changes as a
 * transaction (journal.c) which is committed with write-ahead logging and
 * then applied to the log-structured log region (storage.c).
 *
 * Crash injection points (crash.h) are checked between the steps so that a
 * test can abort the process at a precise moment.
 */
#ifndef JFS_JOURNALFS_H
#define JFS_JOURNALFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "disk_format.h"
#include "journal.h"
#include "util.h"

struct jfs_fs;

/* ---------------- formatting and mounting ---------------- */

typedef struct {
    uint64_t size_bytes;         /* image size to create                    */
    bool     force;              /* overwrite an existing image             */
    bool     verbose;
} jfs_format_opts;

/* Create a new image and write an empty, checked-in filesystem into it. */
int jfs_format(const char *path, const jfs_format_opts *opts, char *err, size_t errsz);

/* Open, validate and recover an image.  `quiet` suppresses recovery output. */
int jfs_mount(const char *path, bool quiet, char *err, size_t errsz);

/* Flush pending state, mark the image clean and close it. */
int jfs_unmount(void);

/* ---------------- state accessors used by the CLI ---------------- */

struct jfs_fs {
    int      fd;                 /* disk image file descriptor              */
    char     path[512];
    bool     mounted;
    bool     dirty;              /* writes since the last clean mark        */
    bool     failed;             /* a commit was applied only partly; the
                                  * image must be replayed before reuse     */
    bool     journal_enabled;    /* false = baseline mode: no write ahead
                                  * log, no fsync barriers (see docs)       */
    bool     journal_truncated;  /* a damaged record cut the journal window
                                  * short: records after it are unreachable */
    uint64_t journal_damaged_block; /* ring block index of that record, for
                                     * diagnostics (valid when the flag is
                                     * set)                                    */

    jfs_superblock sb;
    jfs_checkpoint ckpt;

    jfs_imap imap;               /* in-memory copy of the inode map         */

    /* checkpoint slot handling */
    int      ckpt_slot;          /* slot most recently written (0 or 1)     */
    uint64_t txns_since_ckpt;    /* transactions applied since last ckpt    */
    uint32_t checkpoint_interval;/* write a checkpoint every N transactions  */
    uint32_t commits_done;       /* successful durable commits this run     */
    int      crash_after;        /* 0 = off, else fire after N commits      */

    /* journal state */
    uint64_t journal_head;       /* next journal block to write (relative)  */
    uint64_t journal_lsn;        /* last journal record sequence            */
    uint64_t next_txid;
    uint64_t replayed;           /* transactions replayed at mount          */
    uint64_t last_applied;       /* txid of the newest applied transaction  */

    /* log state */
    uint64_t log_seq;            /* last log record sequence number         */
    uint64_t log_live_bytes;

    /* group commit state (Add-on 1) */
    uint32_t group_commit_batch;   /* 1 = per-transaction sync, >1 = batch group commit */
    uint32_t group_commit_pending; /* un-synced commits currently buffered */

    /* statistics for `df`, benchmarks and tests */
    struct {
        uint64_t txns_committed;
        uint64_t txns_replayed;
        uint64_t redo_ops;
        uint64_t bytes_written;
        uint64_t syncs;
        uint64_t compactions;
        uint64_t journal_blocks_freed;
        uint64_t checkpoint_seq;
        uint64_t group_commits;
        uint64_t group_commit_flushes;
    } stats;

    /* crash injection */
    int crash_mode;              /* JFS_CRASH_NONE or a JFS_CRASH_* value   */
};

/* Mounted filesystem singleton (single image per process). */
struct jfs_fs *jfs_fs_get(void);

/* ---------------- filesystem operations ---------------- */

/* All paths are absolute, normalised paths ("/a/b").  The CLI normalises
 * relative paths against the current directory first. */
int jfs_create(const char *path, uint32_t mode);
int jfs_mkdir(const char *path, uint32_t mode);            /* M5, stub in M1 */
int jfs_read (const char *path, jfs_buf *out);             /* M1 */
int jfs_read_range(const char *path, uint64_t off, uint64_t len, jfs_buf *out);
int jfs_write(const char *path, const void *data, size_t len, uint64_t off,
              bool append, bool do_truncate);
int jfs_readdir(const char *path, void (*emit)(void *, const char *, const jfs_dirent *),
                void *ud);
int jfs_rm(const char *path, bool recursive);
int jfs_rmdir(const char *path);
int jfs_rename(const char *from, const char *to);
int jfs_truncate(const char *path, uint64_t size);
int jfs_stat(const char *path, jfs_inode *out);
int jfs_chmod(const char *path, uint32_t mode);
int jfs_lookup(const char *path, uint32_t *ino_out, jfs_inode *out);
int jfs_exists(const char *path);

/* Path resolver used by every operation.  Follows each component of `path`
 * and returns the inode of the final component.  `parent_out` receives the
 * containing directory when it is not NULL. */
int jfs_resolve(const char *path, uint32_t *ino_out, uint32_t *parent_out,
                char *name_out, size_t namesz);

/* Permission check helper (Layer: permissions). */
int jfs_check_perm(const jfs_inode *inode, uint32_t wanted_bits, const char *what);

/* `df` style free space information. */
int jfs_df(uint64_t *total_bytes, uint64_t *used_bytes,
           uint64_t *used_inodes, uint64_t *max_inodes, uint64_t *log_free);

/* `sync`: flush everything that is still only in memory. */
int jfs_sync(void);

/* Advance the journal window and write a new checkpoint after a transaction
 * (or a batch of them) has been applied.  This is the only place that makes
 * the "applied" state durable, so recovery replays everything newer. */
int jfs_finish_transaction(struct jfs_fs *fs, uint64_t applied_txid);

/* Crash injection that honours --crash-after N (only armed from the Nth
 * commit onwards).  Used by the layers that have an fs handle. */
void jfs_crash_check_fs(const struct jfs_fs *fs, int point, const char *tag);

/* Write the checkpoint and mark the image clean/unclean. */
int jfs_checkpoint_write(bool clean, char *err, size_t errsz);
int jfs_checkpoint_load(struct jfs_fs *fs, jfs_checkpoint *out);

/* Format validation helpers used by mount. */
int jfs_validate_superblock(const jfs_superblock *sb, uint64_t image_size,
                            char *err, size_t errsz);

/* Inode map helpers (inode.c). */
bool jfs_imap_verify(const jfs_imap *map);
int  jfs_imap_publish(struct jfs_fs *fs, jfs_txn *tx);   /* append new imap rec */

/* Write one specific checkpoint structure into the next slot and fsync it.
 * `clean` also updates the superblock clean/dirty flag (filesystem.c). */
int jfs_checkpoint_write_struct(struct jfs_fs *fs, const jfs_checkpoint *ck,
                                bool clean);

/* Inode/map helpers. */
jfs_imap_entry *jfs_imap_lookup(struct jfs_fs *fs, uint32_t ino);
#endif /* JFS_JOURNALFS_H */
