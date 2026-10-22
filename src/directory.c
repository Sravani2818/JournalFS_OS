/*
 * directory.c -- directory blocks.
 *
 * A directory is an inode of type JFS_TYPE_DIR whose data blocks are
 * jfs_dirblk records.  Each block holds up to JFS_DIRENTS_PER_BLOCK
 * name -> inode entries, kept packed at the front of the array so that the
 * on-disk form is deterministic.  Removal compacts by moving the last entry
 * into the hole; this keeps `count` meaningful without a free list.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "checksum.h"
#include "directory.h"
#include "inode.h"
#include "journalfs.h"
#include "storage.h"
#include "util.h"

static void dirblk_finalize(jfs_dirblk *b);

static void dirblk_init(jfs_dirblk *b)
{
    memset(b, 0, sizeof(*b));
    b->magic = JFS_DIRBLK_MAGIC;
    b->count = 0;
    dirblk_finalize(b);
}

static void dirblk_finalize(jfs_dirblk *b)
{
    b->crc = 0;
    b->crc = jfs_crc_struct(b, sizeof(*b), offsetof(jfs_dirblk, crc));
}

int jfs_dirview_open(struct jfs_fs *fs, uint32_t dir_ino, jfs_dirview *dv)
{
    jfs_inode inode;
    uint32_t i;
    int rc;

    if (!dv)
        return JFS_ERR_INVAL;
    memset(dv, 0, sizeof(*dv));

    rc = jfs_inode_load(fs, dir_ino, &inode);
    if (rc != JFS_OK)
        return rc;
    if (inode.type != JFS_TYPE_DIR)
        return JFS_ERR_NOTDIR;
    dv->inode = inode;

    if (inode.nblocks == 0) {
        /* A directory always has at least one (possibly empty) data block. */
        dirblk_init(&dv->blk[0]);
        dv->nblocks = 1;
        dv->dirty_mask = 1u;
        return JFS_OK;
    }

    for (i = 0; i < inode.nblocks && i < JFS_MAX_FILE_BLOCKS; i++) {
        if (inode.addrs[i] == 0)
            return JFS_ERR_CORRUPT;
        rc = jfs_log_read_dirblk(fs, inode.addrs[i], &dv->blk[i]);
        if (rc != JFS_OK)
            return rc;
        if (dv->blk[i].count > JFS_DIRENTS_PER_BLOCK)
            return JFS_ERR_CORRUPT;
        /* every entry in the used prefix must be valid */
        {
            uint32_t k;
            for (k = 0; k < dv->blk[i].count; k++) {
                if (!(dv->blk[i].ents[k].flags & JFS_DE_VALID))
                    return JFS_ERR_CORRUPT;
            }
        }
        dv->nblocks = i + 1;
    }
    if (dv->nblocks == 0) {
        dirblk_init(&dv->blk[0]);
        dv->nblocks = 1;
        dv->dirty_mask = 1u;
    }
    return JFS_OK;
}

void jfs_dirview_free(jfs_dirview *dv)
{
    if (dv)
        memset(dv, 0, sizeof(*dv));
}

int jfs_dirview_lookup(jfs_dirview *dv, const char *name, jfs_dirent *out)
{
    uint32_t i, k;

    if (!dv || !name)
        return JFS_ERR_INVAL;
    for (i = 0; i < dv->nblocks; i++) {
        for (k = 0; k < dv->blk[i].count; k++) {
            const jfs_dirent *e = &dv->blk[i].ents[k];
            if ((e->flags & JFS_DE_VALID) && strcmp(e->name, name) == 0) {
                if (out)
                    *out = *e;
                return JFS_OK;
            }
        }
    }
    return JFS_ERR_NOTFOUND;
}

int jfs_dirview_insert(jfs_dirview *dv, const jfs_dirent *ent)
{
    uint32_t i, k;

    if (!dv || !ent)
        return JFS_ERR_INVAL;
    if (jfs_dirview_lookup(dv, ent->name, NULL) == JFS_OK)
        return JFS_ERR_EXISTS;

    for (i = 0; i < dv->nblocks; i++) {
        if (dv->blk[i].count < JFS_DIRENTS_PER_BLOCK) {
            dv->blk[i].ents[dv->blk[i].count] = *ent;
            dv->blk[i].ents[dv->blk[i].count].flags = JFS_DE_VALID;
            dv->blk[i].count++;
            dirblk_finalize(&dv->blk[i]);
            dv->dirty_mask |= (1u << i);
            return JFS_OK;
        }
    }

    /* grow the directory with a fresh block */
    if (dv->nblocks >= JFS_MAX_FILE_BLOCKS)
        return JFS_ERR_DIRFULL;
    dirblk_init(&dv->blk[dv->nblocks]);
    dv->blk[dv->nblocks].ents[0] = *ent;
    dv->blk[dv->nblocks].ents[0].flags = JFS_DE_VALID;
    dv->blk[dv->nblocks].count = 1;
    dirblk_finalize(&dv->blk[dv->nblocks]);
    dv->dirty_mask |= (1u << dv->nblocks);
    dv->nblocks++;
    (void)k;
    return JFS_OK;
}

int jfs_dirview_remove(jfs_dirview *dv, const char *name, jfs_dirent *removed)
{
    uint32_t i, k;

    if (!dv || !name)
        return JFS_ERR_INVAL;
    for (i = 0; i < dv->nblocks; i++) {
        jfs_dirblk *b = &dv->blk[i];
        for (k = 0; k < b->count; k++) {
            if ((b->ents[k].flags & JFS_DE_VALID) && strcmp(b->ents[k].name, name) == 0) {
                if (removed)
                    *removed = b->ents[k];
                b->count--;
                if (k != b->count)
                    b->ents[k] = b->ents[b->count];   /* compact with the last entry */
                memset(&b->ents[b->count], 0, sizeof(b->ents[b->count]));
                dirblk_finalize(b);
                dv->dirty_mask |= (1u << i);
                return JFS_OK;
            }
        }
    }
    return JFS_ERR_NOTFOUND;
}

uint32_t jfs_dirview_count(const jfs_dirview *dv)
{
    uint32_t i, n = 0;

    for (i = 0; i < dv->nblocks; i++)
        n += dv->blk[i].count;
    return n;
}

bool jfs_dirview_isempty(const jfs_dirview *dv)
{
    return jfs_dirview_count(dv) == 0;
}

int jfs_dirview_to_txn(struct jfs_fs *fs, jfs_txn *tx, const jfs_dirview *dv)
{
    jfs_inode inode = dv->inode;
    uint32_t i;
    int rc;

    if (dv->dirty_mask == 0)
        return JFS_OK;

    for (i = 0; i < dv->nblocks; i++) {
        if (dv->dirty_mask & (1u << i)) {
            rc = jfs_txn_add_dirblk(tx, inode.ino, i, &dv->blk[i]);
            if (rc != JFS_OK)
                return rc;
        }
    }

    inode.nblocks = dv->nblocks;
    inode.size = (uint64_t)dv->nblocks * (uint64_t)sizeof(jfs_dirblk);
    inode.provided_mask = dv->dirty_mask;
    inode.generation++;
    jfs_now(&inode.mtime);
    (void)fs;
    return jfs_txn_add_inode(tx, &inode);
}
