/*
 * checksum.h -- CRC-32C (Castagnoli) used for every integrity field.
 *
 * JournalFS stamps a CRC on every superblock, checkpoint slot, imap, inode,
 * directory block, log record header, log record payload, journal record
 * header and journal record payload.  A record whose CRC does not match is
 * never trusted: it is reported and ignored (or, for the checkpoint, the
 * other slot is used).
 */
#ifndef JFS_CHECKSUM_H
#define JFS_CHECKSUM_H

#include <stdint.h>
#include <stddef.h>

/* crc32c of a byte buffer. */
uint32_t jfs_crc32c(const void *data, size_t len);

/* Same, but starts from a previous value (for incremental hashing). */
uint32_t jfs_crc32c_update(uint32_t crc, const void *data, size_t len);

/* 64 bit FNV-1a style hash, used for the journal window header where a
 * slightly stronger integrity field is worth the few extra cycles. */
uint64_t jfs_hash64(const void *data, size_t len);

/* Helper: checksum a structure whose own `crc` field must be ignored.
 * `crc_off` is the byte offset of the uint32_t crc field inside it. */
uint32_t jfs_crc_struct(const void *data, size_t len, size_t crc_off);

#endif /* JFS_CHECKSUM_H */
