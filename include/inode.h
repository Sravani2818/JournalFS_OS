/*
 * inode.h -- inode map and inode record helpers.
 */
#ifndef JFS_INODE_H
#define JFS_INODE_H

#include <stdint.h>
#include <stdbool.h>

#include "disk_format.h"

struct jfs_fs;

/* Validate the CRC and the entry bounds of a decoded imap record. */
bool jfs_imap_verify(const jfs_imap *map);

/* Find the map entry for `ino`, or NULL (free or out of range). */
jfs_imap_entry *jfs_imap_lookup(struct jfs_fs *fs, uint32_t ino);

/* Lowest free inode number, or 0 when the table is full.  The map is not
 * modified here: publishing happens when the transaction is applied. */
uint32_t jfs_inode_alloc_number(struct jfs_fs *fs, char *err, size_t errsz);

/* Load the newest version of an inode.  Fails with JFS_ERR_NOTFOUND when the
 * inode is free, and JFS_ERR_CORRUPT when the record is damaged. */
int jfs_inode_load(struct jfs_fs *fs, uint32_t ino, jfs_inode *out);

/* Build a fresh inode structure (no disk writes). */
int jfs_inode_init(struct jfs_fs *fs, jfs_inode *out, uint32_t ino, uint16_t type,
                   uint32_t mode);

/* Structural validation of an inode: types, bounds, block references and
 * the rule that the stored bytes must cover the logical size. */
bool jfs_inode_sane(const jfs_inode *inode);

/* Number of bytes of the inode's data that are actually stored. */
uint64_t jfs_inode_stored_bytes(const jfs_inode *inode);

#endif
