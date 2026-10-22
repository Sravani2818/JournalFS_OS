/*
 * disk_image.c -- Layer 6 plumbing: opening the image file, reading and
 *                 writing the superblock, and validating the geometry.
 *
 * The host filesystem is used only to open the image and to move bytes to
 * and from it.  Every JournalFS structure lives inside the image.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "checksum.h"
#include "journalfs.h"
#include "storage.h"
#include "util.h"

int jfs_image_create(const char *path, uint64_t size, bool force, char *err, size_t errsz)
{
    int flags = O_RDWR | O_CREAT;
    int fd;

    if (!force)
        flags |= O_EXCL;
    else
        flags |= O_TRUNC;

    fd = open(path, flags, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            snprintf(err, errsz, "image '%s' already exists (use --force to overwrite)", path);
        else
            snprintf(err, errsz, "cannot create image '%s': %s", path, strerror(errno));
        return (errno == EEXIST) ? JFS_ERR_EXISTSSLOT : JFS_ERR_IO;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        snprintf(err, errsz, "cannot size image '%s' to %llu bytes: %s", path,
                 (unsigned long long)size, strerror(errno));
        close(fd);
        return JFS_ERR_IO;
    }
    return fd;
}

int jfs_image_open(const char *path, char *err, size_t errsz)
{
    int fd = open(path, O_RDWR);

    if (fd < 0) {
        snprintf(err, errsz, "cannot open image '%s': %s", path, strerror(errno));
        return JFS_ERR_IO;
    }
    return fd;
}

int jfs_read_superblock(struct jfs_fs *fs, char *err, size_t errsz)
{
    ssize_t n = jfs_pread_full(fs->fd, &fs->sb, sizeof(fs->sb), 0);

    if (n < 0) {
        snprintf(err, errsz, "cannot read superblock: %s", strerror(errno));
        return JFS_ERR_IO;
    }
    if ((size_t)n != sizeof(fs->sb)) {
        snprintf(err, errsz, "image is smaller than one block (%zd bytes read)", n);
        return JFS_ERR_TRUNCATED;
    }
    if (fs->sb.magic != JFS_SUPER_MAGIC) {
        snprintf(err, errsz, "bad magic 0x%08x (expected 0x%08x): not a JournalFS image",
                 fs->sb.magic, JFS_SUPER_MAGIC);
        return JFS_ERR_BADFMT;
    }
    if (fs->sb.version != JFS_FORMAT_VERSION) {
        snprintf(err, errsz, "unsupported format version %u (this build supports %u)",
                 fs->sb.version, JFS_FORMAT_VERSION);
        return JFS_ERR_BADFMT;
    }
    return JFS_OK;
}

int jfs_validate_superblock(const jfs_superblock *sb, uint64_t image_size,
                            char *err, size_t errsz)
{
    jfs_superblock copy = *sb;

    if (sb->magic != JFS_SUPER_MAGIC) {
        snprintf(err, errsz, "bad magic 0x%08x: not a JournalFS image", sb->magic);
        return JFS_ERR_BADFMT;
    }
    if (sb->version != JFS_FORMAT_VERSION) {
        snprintf(err, errsz, "unsupported format version %u (supported: %u)",
                 sb->version, JFS_FORMAT_VERSION);
        return JFS_ERR_BADFMT;
    }
    copy.crc = 0;
    if (jfs_crc_struct(&copy, sizeof(copy), offsetof(jfs_superblock, crc)) != sb->crc) {
        snprintf(err, errsz, "superblock checksum mismatch: image header is damaged");
        return JFS_ERR_CORRUPT;
    }
    if (sb->block_size != JFS_BLOCK_SIZE) {
        snprintf(err, errsz, "block size %u does not match this build (%u)",
                 sb->block_size, JFS_BLOCK_SIZE);
        return JFS_ERR_BADFMT;
    }
    if (sb->total_blocks != image_size / JFS_BLOCK_SIZE) {
        snprintf(err, errsz,
                 "image is truncated or the wrong file: the superblock "
                 "describes %llu blocks but the file holds %llu",
                 (unsigned long long)sb->total_blocks,
                 (unsigned long long)(image_size / JFS_BLOCK_SIZE));
        return JFS_ERR_TRUNCATED;
    }
    if (sb->checkpoint_block != JFS_CHECKPOINT_BLOCK ||
        sb->journal_block != JFS_JOURNAL_BLOCK ||
        sb->journal_blocks != JFS_JOURNAL_BLOCKS ||
        sb->log_block != JFS_LOG_BLOCK) {
        snprintf(err, errsz, "image geometry is inconsistent with this build");
        return JFS_ERR_BADFMT;
    }
    if (sb->log_block + sb->log_blocks != sb->total_blocks) {
        snprintf(err, errsz, "log region does not reach the end of the image");
        return JFS_ERR_BADFMT;
    }
    if (sb->log_blocks / 2 < JFS_MIN_LOG_HALF_BLOCKS) {
        snprintf(err, errsz, "log region is too small (%llu blocks)",
                 (unsigned long long)sb->log_blocks);
        return JFS_ERR_BADFMT;
    }
    if (sb->max_inodes != JFS_MAX_INODES || sb->max_file_bytes != JFS_MAX_FILE_BYTES) {
        snprintf(err, errsz, "limits recorded in the superblock differ from this build");
        return JFS_ERR_BADFMT;
    }
    return JFS_OK;
}

int jfs_write_superblock(struct jfs_fs *fs, bool sync)
{
    ssize_t n;

    fs->sb.crc = 0;
    fs->sb.crc = jfs_crc_struct(&fs->sb, sizeof(fs->sb), offsetof(jfs_superblock, crc));
    n = jfs_pwrite_full(fs->fd, &fs->sb, sizeof(fs->sb), 0);
    if (n < 0 || (size_t)n != sizeof(fs->sb))
        return JFS_ERR_IO;
    fs->stats.bytes_written += sizeof(fs->sb);
    if (sync) {
        if (fsync(fs->fd) != 0)
            return JFS_ERR_IO;
        fs->stats.syncs++;
    }
    return JFS_OK;
}

int jfs_superblock_set_clean(struct jfs_fs *fs, bool clean, bool sync)
{
    uint32_t old = fs->sb.flags;

    if (clean)
        fs->sb.flags |= JFS_SB_CLEAN;
    else
        fs->sb.flags &= ~(uint32_t)JFS_SB_CLEAN;

    if (jfs_write_superblock(fs, sync) != JFS_OK) {
        fs->sb.flags = old;
        return JFS_ERR_IO;
    }
    return JFS_OK;
}

int jfs_image_size(int fd, uint64_t *out, char *err, size_t errsz)
{
    off_t end = lseek(fd, 0, SEEK_END);

    if (end < 0) {
        snprintf(err, errsz, "cannot determine image size: %s", strerror(errno));
        return JFS_ERR_IO;
    }
    *out = (uint64_t)end;
    return JFS_OK;
}

void jfs_layout_for_size(uint64_t size_bytes, jfs_superblock *sb)
{
    memset(sb, 0, sizeof(*sb));
    sb->magic          = JFS_SUPER_MAGIC;
    sb->version        = JFS_FORMAT_VERSION;
    sb->block_size     = JFS_BLOCK_SIZE;
    sb->total_blocks   = size_bytes / JFS_BLOCK_SIZE;
    sb->checkpoint_block = JFS_CHECKPOINT_BLOCK;
    sb->journal_block  = JFS_JOURNAL_BLOCK;
    sb->journal_blocks = JFS_JOURNAL_BLOCKS;
    sb->log_block      = JFS_LOG_BLOCK;
    sb->log_blocks     = sb->total_blocks - JFS_LOG_BLOCK;
    sb->max_inodes     = JFS_MAX_INODES;
    sb->max_file_bytes = JFS_MAX_FILE_BYTES;
    sb->flags          = JFS_SB_CLEAN;
    sb->image_id       = 0;   /* filled in by format */
    jfs_now(&sb->created_at);
}
