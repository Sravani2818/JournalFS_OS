/*
 * corrupt_tool.h -- C implementation of corrupt.py routines for JournalFS corruption testing.
 */
#ifndef JFS_CORRUPT_TOOL_H
#define JFS_CORRUPT_TOOL_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "checksum.h"
#include "disk_format.h"

static inline int jfs_corrupt_sb_flip(const char *path, uint64_t offset)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    uint8_t b = 0;
    if (pread(fd, &b, 1, (off_t)offset) != 1) { close(fd); return -1; }
    b ^= 0x01;
    if (pwrite(fd, &b, 1, (off_t)offset) != 1) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_sb_truncate(const char *path, uint64_t blocks)
{
    return truncate(path, (off_t)(blocks * JFS_BLOCK_SIZE));
}

static inline int jfs_corrupt_ckpt_zero(const char *path, int slot)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    off_t off = (off_t)(sb.checkpoint_block + (uint64_t)slot) * JFS_BLOCK_SIZE;
    uint8_t garbage[JFS_BLOCK_SIZE];
    memset(garbage, 0x5a, sizeof(garbage));
    if (pwrite(fd, garbage, sizeof(garbage), off) != sizeof(garbage)) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_ckpt_both(const char *path)
{
    if (jfs_corrupt_ckpt_zero(path, 0) != 0) return -1;
    return jfs_corrupt_ckpt_zero(path, 1);
}

/* Locate journal record at win_index */
static inline int jfs_find_jrec_block(int fd, const jfs_superblock *sb, const jfs_checkpoint *ck,
                                      uint64_t target_win_index, uint64_t *out_blk, jfs_journal_rec_hdr *out_hdr)
{
    uint32_t jblocks = sb->journal_blocks;
    uint32_t used = 0;

    while (used < jblocks) {
        uint64_t rel = (ck->journal_start + used) % jblocks;
        uint64_t blk = sb->journal_block + rel;
        jfs_journal_rec_hdr hdr;
        if (pread(fd, &hdr, sizeof(hdr), (off_t)blk * JFS_BLOCK_SIZE) != sizeof(hdr))
            break;
        if (hdr.magic != JFS_JREC_MAGIC || hdr.version != JFS_FORMAT_VERSION)
            break;
        if (jfs_crc_struct(&hdr, sizeof(hdr), offsetof(jfs_journal_rec_hdr, hdr_crc)) != hdr.hdr_crc)
            break;
        if (hdr.nblocks == 0 || used + hdr.nblocks > jblocks)
            break;
        if (used >= ck->journal_used && hdr.txid <= ck->applied_txid)
            break;

        if (used == target_win_index) {
            if (out_blk) *out_blk = blk;
            if (out_hdr) *out_hdr = hdr;
            return 0;
        }
        used += hdr.nblocks;
    }
    return -1;
}

static inline int jfs_get_best_ckpt(int fd, const jfs_superblock *sb, int *slot_out, jfs_checkpoint *out)
{
    int best_slot = -1;
    jfs_checkpoint best_ck;
    for (int s = 0; s < 2; s++) {
        off_t off = (off_t)(sb->checkpoint_block + (uint64_t)s) * JFS_BLOCK_SIZE;
        jfs_checkpoint ck;
        if (pread(fd, &ck, sizeof(ck), off) == sizeof(ck)) {
            if (ck.magic == JFS_CKPT_MAGIC) {
                if (jfs_crc_struct(&ck, sizeof(ck), offsetof(jfs_checkpoint, crc)) == ck.crc) {
                    if (best_slot < 0 || ck.seq > best_ck.seq) {
                        best_slot = s;
                        best_ck = ck;
                    }
                }
            }
        }
    }
    if (best_slot < 0) return -1;
    if (slot_out) *slot_out = best_slot;
    if (out) *out = best_ck;
    return 0;
}

static inline int jfs_corrupt_jrec_flip_payload(const char *path, uint64_t index)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    jfs_checkpoint ck;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    if (jfs_get_best_ckpt(fd, &sb, NULL, &ck) != 0) { close(fd); return -1; }

    uint64_t blk = 0;
    jfs_journal_rec_hdr hdr;
    if (jfs_find_jrec_block(fd, &sb, &ck, index, &blk, &hdr) != 0) { close(fd); return -1; }

    off_t off = (off_t)blk * JFS_BLOCK_SIZE + sizeof(hdr);
    uint8_t b = 0;
    if (pread(fd, &b, 1, off) != 1) { close(fd); return -1; }
    b ^= 0xFF;
    if (pwrite(fd, &b, 1, off) != 1) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_jrec_flip_header(const char *path, uint64_t index)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    jfs_checkpoint ck;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    if (jfs_get_best_ckpt(fd, &sb, NULL, &ck) != 0) { close(fd); return -1; }

    uint64_t blk = 0;
    jfs_journal_rec_hdr hdr;
    if (jfs_find_jrec_block(fd, &sb, &ck, index, &blk, &hdr) != 0) { close(fd); return -1; }

    off_t off = (off_t)blk * JFS_BLOCK_SIZE + offsetof(jfs_journal_rec_hdr, hdr_crc);
    uint8_t b = 0;
    if (pread(fd, &b, 1, off) != 1) { close(fd); return -1; }
    b ^= 0xFF;
    if (pwrite(fd, &b, 1, off) != 1) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_jrec_txid(const char *path, uint64_t index, uint64_t new_txid)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    jfs_checkpoint ck;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    if (jfs_get_best_ckpt(fd, &sb, NULL, &ck) != 0) { close(fd); return -1; }

    uint64_t blk = 0;
    jfs_journal_rec_hdr hdr;
    if (jfs_find_jrec_block(fd, &sb, &ck, index, &blk, &hdr) != 0) { close(fd); return -1; }

    hdr.txid = new_txid;
    hdr.hdr_crc = 0;
    hdr.hdr_crc = jfs_crc_struct(&hdr, sizeof(hdr), offsetof(jfs_journal_rec_hdr, hdr_crc));

    off_t off = (off_t)blk * JFS_BLOCK_SIZE;
    if (pwrite(fd, &hdr, sizeof(hdr), off) != sizeof(hdr)) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_jrec_type(const char *path, uint64_t index, uint16_t new_type)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    jfs_checkpoint ck;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    if (jfs_get_best_ckpt(fd, &sb, NULL, &ck) != 0) { close(fd); return -1; }

    uint64_t blk = 0;
    jfs_journal_rec_hdr hdr;
    if (jfs_find_jrec_block(fd, &sb, &ck, index, &blk, &hdr) != 0) { close(fd); return -1; }

    hdr.type = new_type;
    hdr.hdr_crc = 0;
    hdr.hdr_crc = jfs_crc_struct(&hdr, sizeof(hdr), offsetof(jfs_journal_rec_hdr, hdr_crc));

    off_t off = (off_t)blk * JFS_BLOCK_SIZE;
    if (pwrite(fd, &hdr, sizeof(hdr), off) != sizeof(hdr)) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_jrec_oversize(const char *path, uint64_t index)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    jfs_checkpoint ck;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    if (jfs_get_best_ckpt(fd, &sb, NULL, &ck) != 0) { close(fd); return -1; }

    uint64_t blk = 0;
    jfs_journal_rec_hdr hdr;
    if (jfs_find_jrec_block(fd, &sb, &ck, index, &blk, &hdr) != 0) { close(fd); return -1; }

    hdr.nblocks = 0xFFFF;
    hdr.hdr_crc = 0;
    hdr.hdr_crc = jfs_crc_struct(&hdr, sizeof(hdr), offsetof(jfs_journal_rec_hdr, hdr_crc));

    off_t off = (off_t)blk * JFS_BLOCK_SIZE;
    if (pwrite(fd, &hdr, sizeof(hdr), off) != sizeof(hdr)) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_imap_flip(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    jfs_checkpoint ck;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    if (jfs_get_best_ckpt(fd, &sb, NULL, &ck) != 0) { close(fd); return -1; }

    uint64_t addr = ck.imap_addr;
    uint64_t half = addr >> 32;
    uint64_t off_in_half = addr & 0xFFFFFFFFu;
    uint64_t blk = sb.log_block + half * (sb.log_blocks / 2u) + off_in_half;

    off_t off = (off_t)blk * JFS_BLOCK_SIZE + sizeof(jfs_log_rec_hdr);
    uint8_t b = 0;
    if (pread(fd, &b, 1, off) != 1) { close(fd); return -1; }
    b ^= 0xFF;
    if (pwrite(fd, &b, 1, off) != 1) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_inode_kill(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    jfs_checkpoint ck;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    if (jfs_get_best_ckpt(fd, &sb, NULL, &ck) != 0) { close(fd); return -1; }

    uint64_t half = ck.log_half;
    uint64_t blk = sb.log_block + half * (sb.log_blocks / 2u) + 1;

    off_t off = (off_t)blk * JFS_BLOCK_SIZE;
    uint8_t zero[JFS_BLOCK_SIZE];
    memset(zero, 0, sizeof(zero));
    if (pwrite(fd, zero, sizeof(zero), off) != sizeof(zero)) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_corrupt_journal_start(const char *path, uint64_t value)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    jfs_superblock sb;
    int slot = 0;
    jfs_checkpoint ck;
    if (pread(fd, &sb, sizeof(sb), 0) != sizeof(sb)) { close(fd); return -1; }
    if (jfs_get_best_ckpt(fd, &sb, &slot, &ck) != 0) { close(fd); return -1; }

    ck.journal_start = value;
    ck.crc = 0;
    ck.crc = jfs_crc_struct(&ck, sizeof(ck), offsetof(jfs_checkpoint, crc));

    off_t off = (off_t)(sb.checkpoint_block + (uint64_t)slot) * JFS_BLOCK_SIZE;
    if (pwrite(fd, &ck, sizeof(ck), off) != sizeof(ck)) { close(fd); return -1; }
    close(fd);
    return 0;
}

static inline int jfs_write_random_file(const char *path, size_t size)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    uint8_t buf[1024];
    size_t written = 0;
    uint32_t seed = 12345;
    while (written < size) {
        size_t chunk = sizeof(buf);
        if (written + chunk > size) chunk = size - written;
        for (size_t i = 0; i < chunk; i++) {
            seed = seed * 1103515245 + 12345;
            buf[i] = (uint8_t)(seed >> 16);
        }
        if (write(fd, buf, chunk) != (ssize_t)chunk) { close(fd); return -1; }
        written += chunk;
    }
    close(fd);
    return 0;
}

#endif /* JFS_CORRUPT_TOOL_H */
