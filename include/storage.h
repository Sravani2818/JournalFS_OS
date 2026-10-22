/*
 * storage.h -- Layer 5, the log-structured storage engine.
 *
 * The log region is split into two equal halves.  New versions of records
 * (inode, file data, directory block, imap) are appended to the active half;
 * nothing is ever overwritten in place.  When the active half fills up the
 * storage engine compacts: it copies the live records into the inactive
 * half, writes a new checkpoint describing the new half, and only then
 * makes the new half active.  The old half stays intact until the new
 * checkpoint is durable, so a crash during compaction always leaves one
 * usable half.
 */
#ifndef JFS_STORAGE_H
#define JFS_STORAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "disk_format.h"

struct jfs_fs;

/* Absolute byte offset of a log address inside the image. */
uint64_t jfs_log_off(const struct jfs_fs *fs, uint64_t addr);

/* Append a typed record; returns its log address in *addr. */
int jfs_log_append(struct jfs_fs *fs, uint16_t type, uint32_t obj_ino,
                   uint32_t block_index, const void *payload, uint32_t payload_len,
                   uint64_t *addr);

/* Read a record and validate magic/version/CRC/image id.  `buf` must be big
 * enough for the payload; the record type and length are returned to
 * the caller through pointers that may be NULL. */
int jfs_log_read(struct jfs_fs *fs, uint64_t addr, void *buf, size_t bufsz,
                 uint16_t *type, uint32_t *len);

/* Convenience readers used by the filesystem layer. */
int jfs_log_read_inode(struct jfs_fs *fs, uint64_t addr, jfs_inode *out);
int jfs_log_read_imap(struct jfs_fs *fs, uint64_t addr, jfs_imap *out);
int jfs_log_read_dirblk(struct jfs_fs *fs, uint64_t addr, jfs_dirblk *out);

/* Compact the log: copy live records into the inactive half, then flip the
 * checkpoint over to it.  Returns JFS_OK or an error. */
int jfs_log_compact(struct jfs_fs *fs);

/* Reclaim space actually used by live records (called after compaction). */
uint64_t jfs_log_live_bytes(const struct jfs_fs *fs);

/* Checkpoint slots (checkpoint.c).  Slots are double buffered: a torn or
 * half written slot simply fails its CRC and the other slot is used. */
int jfs_checkpoint_load_slot(struct jfs_fs *fs, int slot, jfs_checkpoint *out);
int jfs_checkpoint_load_best(struct jfs_fs *fs, int *slot_out, jfs_checkpoint *out);
int jfs_checkpoint_slot_block(const struct jfs_fs *fs, int slot);
/* Write one slot with or without an fsync (checkpoint.c). */
int jfs_checkpoint_write_slot(struct jfs_fs *fs, const jfs_checkpoint *ck,
                              int slot, bool sync);

/* Free-space accounting (blocks). */
uint64_t jfs_log_free_blocks(const struct jfs_fs *fs);
uint64_t jfs_log_half_blocks(const struct jfs_fs *fs);

#endif /* JFS_STORAGE_H */
