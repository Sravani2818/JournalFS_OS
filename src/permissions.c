/*
 * permissions.c -- the simulated permission model.
 */
#include <stdio.h>

#include "permissions.h"
#include "util.h"

int jfs_permission_check(const jfs_inode *inode, uint32_t wanted, const char *what)
{
    uint32_t allowed = 0;

    if (!inode)
        return JFS_ERR_INVAL;

    /* Single user model: the simulated process always runs as the owner, so
     * the owner triad decides.  Group/other bits are stored and displayed so
     * that the on-disk format looks like a real inode, but they are not used
     * for access decisions in this simulator. */
    if (wanted & JFS_R_OK)
        allowed |= (inode->mode & JFS_S_IRUSR) ? JFS_R_OK : 0u;
    if (wanted & JFS_W_OK)
        allowed |= (inode->mode & JFS_S_IWUSR) ? JFS_W_OK : 0u;
    if (wanted & JFS_X_OK)
        allowed |= (inode->mode & JFS_S_IXUSR) ? JFS_X_OK : 0u;

    if ((wanted & ~allowed) != 0) {
        (void)what;
        return JFS_ERR_PERM;
    }
    return JFS_OK;
}

void jfs_mode_string(uint32_t mode, char *out, size_t outsz)
{
    static const char *rwx = "rwx";
    size_t i;

    if (outsz < 10)
        return;
    for (i = 0; i < 9; i++) {
        uint32_t bits = (mode >> (8 - i)) & 1u;
        out[i] = bits ? rwx[i % 3] : '-';
    }
    out[9] = '\0';
}
