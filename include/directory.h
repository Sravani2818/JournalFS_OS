/*
 * directory.h -- directory blocks and directory entry handling.
 *
 * A directory is an inode of type JFS_TYPE_DIR whose data blocks are
 * jfs_dirblk records holding name -> inode number mappings.  File contents
 * are never stored inside a directory entry.
 */
#ifndef JFS_DIRECTORY_H
#define JFS_DIRECTORY_H

#include <stdint.h>
#include <stdbool.h>

#include "disk_format.h"
#include "journal.h"

struct jfs_fs;

/* A directory opened for modification: the blocks are read into memory,
 * changed, and written back as new append-only versions. */
typedef struct {
    jfs_inode  inode;                       /* the directory inode         */
    jfs_dirblk blk[JFS_MAX_FILE_BLOCKS];
    uint32_t   nblocks;
    uint32_t   dirty_mask;                  /* blocks changed in memory    */
} jfs_dirview;

int  jfs_dirview_open(struct jfs_fs *fs, uint32_t dir_ino, jfs_dirview *dv);
void jfs_dirview_free(jfs_dirview *dv);
int  jfs_dirview_lookup(jfs_dirview *dv, const char *name, jfs_dirent *out);
int  jfs_dirview_insert(jfs_dirview *dv, const jfs_dirent *ent);
int  jfs_dirview_remove(jfs_dirview *dv, const char *name, jfs_dirent *removed);
uint32_t jfs_dirview_count(const jfs_dirview *dv);
bool jfs_dirview_isempty(const jfs_dirview *dv);
/* Add the dirty blocks plus the updated directory inode to a transaction. */
int  jfs_dirview_to_txn(struct jfs_fs *fs, jfs_txn *tx, const jfs_dirview *dv);

#endif
