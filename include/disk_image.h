/*
 * disk_image.h -- host-side access to the disk image file (Layer 6).
 *
 * Only byte movement and the superblock live here: the host filesystem is
 * used to open the image, to move blocks and to order writes with fsync().
 * No JournalFS object is ever stored as a host file.
 */
#ifndef JFS_DISK_IMAGE_H
#define JFS_DISK_IMAGE_H

#include <stdint.h>
#include <stdbool.h>

#include "disk_format.h"

struct jfs_fs;

/* Create (or with `force`, truncate) an image of exactly `size` bytes. */
int jfs_image_create(const char *path, uint64_t size, bool force,
                     char *err, size_t errsz);
/* Open an existing image read/write. */
int jfs_image_open(const char *path, char *err, size_t errsz);
/* Size of an open image in bytes. */
int jfs_image_size(int fd, uint64_t *out, char *err, size_t errsz);

/* Read and sanity check the superblock into fs->sb. */
int jfs_read_superblock(struct jfs_fs *fs, char *err, size_t errsz);
/* Full validation of a superblock (magic, version, CRC, geometry, limits). */
int jfs_validate_superblock(const jfs_superblock *sb, uint64_t image_size,
                            char *err, size_t errsz);
/* Write fs->sb back (recomputing its CRC), optionally with an fsync. */
int jfs_write_superblock(struct jfs_fs *fs, bool sync);
/* Update and persist the clean/dirty flag. */
int jfs_superblock_set_clean(struct jfs_fs *fs, bool clean, bool sync);

/* Fill a superblock with the geometry for an image of `size_bytes`. */
void jfs_layout_for_size(uint64_t size_bytes, jfs_superblock *sb);

#endif /* JFS_DISK_IMAGE_H */
