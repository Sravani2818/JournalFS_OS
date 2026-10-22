/*
 * checkpoint.c -- checkpoint slots.
 *
 * There are two checkpoint blocks.  Every checkpoint write goes to the slot
 * that is NOT the current one, with a sequence number one higher than the
 * current one, and is followed by an fsync.  A crash half way through the
 * write therefore leaves one perfectly good checkpoint (the older sequence)
 * and one torn one that fails its CRC; the loader simply takes the highest
 * valid sequence number.  This is the classic double buffered superblock /
 * "torn write" defence.
 *
 * The checkpoint is the only place that records how far the journal has been
 * applied (applied_txid), which log half is active, where the newest imap
 * record is, and how much of the journal window is still needed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "checksum.h"
#include "disk_image.h"
#include "journalfs.h"
#include "storage.h"
#include "util.h"

int jfs_checkpoint_slot_block(const struct jfs_fs *fs, int slot)
{
    return (int)fs->sb.checkpoint_block + slot;
}

static bool checkpoint_sane(const struct jfs_fs *fs, const jfs_checkpoint *ck)
{
    jfs_checkpoint copy = *ck;
    uint64_t half = fs->sb.log_blocks / 2u;

    if (ck->magic != JFS_CKPT_MAGIC)
        return false;
    if (ck->version != JFS_FORMAT_VERSION)
        return false;
    if (ck->seq == 0)
        return false;
    copy.crc = 0;
    if (jfs_crc_struct(&copy, sizeof(copy), offsetof(jfs_checkpoint, crc)) != ck->crc)
        return false;
    if (ck->log_half > 1)
        return false;
    if (ck->log_head == 0 || ck->log_head > half)
        return false;   /* block 0 is reserved as the null address */
    if (ck->journal_start >= fs->sb.journal_blocks)
        return false;
    if (ck->journal_used > fs->sb.journal_blocks)
        return false;
    if (ck->imap_addr == 0)
        return false;
    if (((ck->imap_addr >> 32) & 0xFFFFFFFFu) > 1)
        return false;
    if ((ck->imap_addr & 0xFFFFFFFFu) >= half)
        return false;
    if (ck->next_txid == 0)
        return false;
    if (ck->applied_txid >= ck->next_txid)
        return false;
    if (ck->n_inodes > fs->sb.max_inodes)
        return false;
    return true;
}

int jfs_checkpoint_load_slot(struct jfs_fs *fs, int slot, jfs_checkpoint *out)
{
    jfs_checkpoint ck;
    ssize_t n = jfs_pread_full(fs->fd, &ck, sizeof(ck),
                               (off_t)(jfs_checkpoint_slot_block(fs, slot) * JFS_BLOCK_SIZE));

    if (n < 0)
        return JFS_ERR_IO;
    if ((size_t)n != sizeof(ck))
        return JFS_ERR_TRUNCATED;
    if (!checkpoint_sane(fs, &ck))
        return JFS_ERR_CORRUPT;
    *out = ck;
    return JFS_OK;
}

int jfs_checkpoint_load_best(struct jfs_fs *fs, int *slot_out, jfs_checkpoint *out)
{
    jfs_checkpoint a, b;
    int ra = jfs_checkpoint_load_slot(fs, 0, &a);
    int rb = jfs_checkpoint_load_slot(fs, 1, &b);

    if (ra != JFS_OK && rb != JFS_OK)
        return JFS_ERR_CORRUPT;
    if (ra == JFS_OK && rb != JFS_OK) {
        if (slot_out)
            *slot_out = 0;
        *out = a;
        return JFS_OK;
    }
    if (rb == JFS_OK && ra != JFS_OK) {
        if (slot_out)
            *slot_out = 1;
        *out = b;
        return JFS_OK;
    }
    if (a.seq >= b.seq) {
        if (slot_out)
            *slot_out = 0;
        *out = a;
    } else {
        if (slot_out)
            *slot_out = 1;
        *out = b;
    }
    return JFS_OK;
}

int jfs_checkpoint_write_slot(struct jfs_fs *fs, const jfs_checkpoint *ck,
                              int slot, bool sync)
{
    jfs_checkpoint copy = *ck;
    ssize_t n;

    copy.crc = 0;
    copy.crc = jfs_crc_struct(&copy, sizeof(copy), offsetof(jfs_checkpoint, crc));

    n = jfs_pwrite_full(fs->fd, &copy, sizeof(copy),
                        (off_t)(jfs_checkpoint_slot_block(fs, slot) * JFS_BLOCK_SIZE));
    if (n < 0 || (size_t)n != sizeof(copy))
        return JFS_ERR_IO;
    fs->stats.bytes_written += sizeof(copy);

    if (sync) {
        if (fsync(fs->fd) != 0)
            return JFS_ERR_IO;
        fs->stats.syncs++;
    }
    return JFS_OK;
}

int jfs_checkpoint_write_struct(struct jfs_fs *fs, const jfs_checkpoint *ck, bool clean)
{
    int slot = (fs->ckpt_slot + 1) % 2;
    int rc = jfs_checkpoint_write_slot(fs, ck, slot, true);

    if (rc != JFS_OK)
        return rc;
    fs->ckpt_slot = slot;
    fs->stats.checkpoint_seq = ck->seq;
    if (clean) {
        rc = jfs_superblock_set_clean(fs, true, true);
        if (rc != JFS_OK)
            return rc;
    }
    return JFS_OK;
}

/* Public wrapper: persist the current in-memory checkpoint with a new
 * sequence number (used by `sync` and by unmount). */
int jfs_checkpoint_write(bool clean, char *err, size_t errsz)
{
    struct jfs_fs *fs = jfs_fs_get();
    jfs_checkpoint ck;

    if (!fs || !fs->mounted) {
        if (err)
            snprintf(err, errsz, "no filesystem is mounted");
        return JFS_ERR_NOTMOUNTED;
    }
    ck = fs->ckpt;
    ck.seq += 1;
    ck.applied_txid = fs->last_applied;
    ck.next_txid = fs->next_txid;
    ck.n_inodes = fs->imap.count;
    ck.live_bytes = fs->log_live_bytes;
    ck.log_seq = fs->log_seq;
    ck.replayed_total = fs->replayed;

    if (jfs_checkpoint_write_struct(fs, &ck, clean) != JFS_OK) {
        if (err)
            snprintf(err, errsz, "cannot write checkpoint: %s", strerror(errno));
        return JFS_ERR_IO;
    }
    fs->ckpt = ck;
    return JFS_OK;
}
