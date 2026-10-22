/*
 * crash.h -- deterministic crash injection points.
 *
 * A crash point is selected with `--crash <name>` on the command line or the
 * JFS_CRASH environment variable.  When execution reaches the matching point
 * the process terminates *immediately* with _exit(), without flushing user
 * space buffers, exactly like a power loss: only the data that already
 * reached the image through a successful write() + fsync() survives.
 *
 * Crash points (see docs/journal_protocol.md for the expected result of each):
 *
 *   none          no crash (default)
 *   pre_write     after the redo operations were built, before any journal
 *                 record reached the image
 *   post_write    journal OP records written but NOT synced
 *   post_sync     journal OP records written and synced, no commit record
 *   post_commit   commit record written but NOT synced
 *   post_commit_sync  commit record written and synced, changes not applied
 *   post_apply    changes applied to the log, checkpoint not updated
 *   after_imap    imap record appended, bitmap/inode files not yet written
 *   checkpoint_mid    halfway through a checkpoint update
 *   checkpoint_post    checkpoint written, previous slot not yet invalidated
 *   compact_mid   log compaction copied some records, no new checkpoint yet
 *   compact_post  new half filled, new checkpoint not yet durable
 *   recover_mid   during recovery, before committed transactions are applied
 *   pre_format    before the image is created (leaves the old image alone)
 */
#ifndef JFS_CRASH_H
#define JFS_CRASH_H

#include <stdbool.h>

typedef enum {
    JFS_CRASH_NONE = 0,
    JFS_CRASH_PRE_WRITE,
    JFS_CRASH_POST_WRITE,
    JFS_CRASH_POST_SYNC,
    JFS_CRASH_POST_COMMIT,
    JFS_CRASH_POST_COMMIT_SYNC,
    JFS_CRASH_POST_APPLY,
    JFS_CRASH_AFTER_IMAP,
    JFS_CRASH_CHECKPOINT_MID,
    JFS_CRASH_CHECKPOINT_POST,
    JFS_CRASH_COMPACT_MID,
    JFS_CRASH_COMPACT_POST,
    JFS_CRASH_RECOVER_MID,
    JFS_CRASH_PRE_FORMAT,
    JFS_CRASH__COUNT
} jfs_crash_point;

/* Parse a crash point name.  Returns JFS_CRASH__COUNT for an unknown name. */
jfs_crash_point jfs_crash_parse(const char *name);
const char    *jfs_crash_name(jfs_crash_point p);

/* Set the active crash point (JFS_CRASH_NONE disables injection). */
void jfs_crash_set(jfs_crash_point p);
jfs_crash_point jfs_crash_get(void);

/* Called at each stage.  If the active crash point matches `p` the process
 * exits immediately with status 99, printing one line to stderr.  `tag` is a
 * free form string used in the message (file name, txid, ...). */
void jfs_crash_check(jfs_crash_point p, const char *tag);

/* Number of times a crash point was hit (useful for self tests). */
unsigned jfs_crash_hits(jfs_crash_point p);

/* Parse JFS_CRASH / JFS_CRASH_AFTER environment variables. */
void jfs_crash_init_from_env(void);

#endif /* JFS_CRASH_H */
