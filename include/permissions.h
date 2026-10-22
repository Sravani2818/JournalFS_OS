/*
 * permissions.h -- the simulated, single user permission model.
 *
 * JournalFS simulates one user (uid 1000, gid 1000) plus the "root of the
 * simulator" (uid 0).  Every process runs as the simulated owner, so a file
 * is checked against its owner bits.  This is a model for teaching and
 * testing, not a security boundary: it cannot and does not prevent the real
 * operating system user from reading the image file.
 */
#ifndef JFS_PERMISSIONS_H
#define JFS_PERMISSIONS_H

#include <stdint.h>
#include <stddef.h>

#include "disk_format.h"

/* Permission class bits. */
#define JFS_R_OK 4u
#define JFS_W_OK 2u
#define JFS_X_OK 1u

/* Check `wanted` (JFS_*_OK) against the simulated credentials. */
int jfs_permission_check(const jfs_inode *inode, uint32_t wanted, const char *what);

/* "rwxr-xr-x" style rendering. */
void jfs_mode_string(uint32_t mode, char *out, size_t outsz);

#endif
