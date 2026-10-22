/*
 * inode.c -- the inode map and inode records.
 *
 * The inode map is a single log record holding one entry per inode number:
 * {state, log address of the newest inode record}.  It is rewritten (a new
 * version appended, never patched in place) at the end of any transaction
 * that creates, updates or deletes an inode, and the checkpoint stores the
 * address of the newest version.  Mounting the filesystem therefore starts
 * by reading exactly one record: the map.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "checksum.h"
#include "inode.h"
#include "journalfs.h"
#include "storage.h"
#include "util.h"

bool jfs_imap_verify(const jfs_imap *map)
{
    uint32_t i;
    uint32_t used = 0;
    jfs_imap copy;

    if (map->magic != JFS_IMAP_MAGIC)
        return false;
    if (map->count > JFS_MAX_INODES)
        return false;
    if (map->next_ino == 0)
        return false;

    copy = *map;
    copy.crc = 0;
    if (jfs_crc_struct(&copy, sizeof(copy), offsetof(jfs_imap, crc)) != map->crc)
        return false;

    for (i = 0; i < JFS_MAX_INODES; i++) {
        const jfs_imap_entry *e = &map->entries[i];
        if (e->ino != 0 && e->ino != i + 1u)
            return false;                 /* entry parked in a foreign slot */
        if (e->state == JFS_IMAP_USED) {
            used++;
            if (e->addr == 0)
                return false;             /* a used inode must have an address */
            if (e->ino == 0)
                return false;
        } else if (e->state == JFS_IMAP_FREE) {
            if (e->addr != 0)
                return false;
        } else {
            return false;                 /* unknown state value */
        }
    }
    if (used != map->count)               /* count must agree with the entries */
        return false;
    return true;
}

/*
 * The map is a fixed table indexed by inode number: entries[ino - 1] belongs
 * to inode `ino`, and an `ino` field of 0 means the slot was never used.
 * Indexing (instead of searching) keeps lookup O(1) and makes a corrupted
 * map detectable: an entry whose number is neither 0 nor its own index is
 * rejected by jfs_imap_verify().
 */
jfs_imap_entry *jfs_imap_lookup(struct jfs_fs *fs, uint32_t ino)
{
    jfs_imap_entry *e;

    if (!fs || ino == 0 || ino > JFS_MAX_INODES)
        return NULL;
    e = &fs->imap.entries[ino - 1u];
    /* A never-used slot (ino 0) is returned as well: creating a file needs
     * the slot before the inode number has been published in the map.
     * Callers that require a live inode check `state` (see jfs_inode_load). */
    if (e->ino != 0 && e->ino != ino)
        return NULL;
    return e;
}

uint32_t jfs_inode_alloc_number(struct jfs_fs *fs, char *err, size_t errsz)
{
    uint32_t i;

    /* Deterministic reuse of the lowest free slot: this keeps the on-disk
     * layout reproducible, which makes the crash tests easy to reason about. */
    for (i = 0; i < JFS_MAX_INODES; i++) {
        if (fs->imap.entries[i].ino == 0 || fs->imap.entries[i].state == JFS_IMAP_FREE)
            return (uint32_t)i + 1u;
    }
    if (err)
        snprintf(err, errsz, "inode table is full (%u inodes)", JFS_MAX_INODES);
    return 0;
}

bool jfs_inode_sane(const jfs_inode *inode)
{
    uint64_t stored = 0;
    uint32_t i;

    if (inode->ino == 0)
        return false;
    if (inode->type != JFS_TYPE_FILE && inode->type != JFS_TYPE_DIR)
        return false;
    if (inode->mode & ~(uint32_t)JFS_PERM_BITS)
        return false;
    if (inode->size > JFS_MAX_FILE_BYTES)
        return false;
    if (inode->nblocks > JFS_MAX_FILE_BLOCKS)
        return false;
    if (inode->flags & ~(uint32_t)(JFS_INODE_FREE))
        return false;
    for (i = 0; i < JFS_MAX_FILE_BLOCKS; i++) {
        if (i < inode->nblocks) {
            if (inode->blklen[i] > JFS_DATA_BYTES)
                return false;
            stored += inode->blklen[i];
            if (inode->blklen[i] != 0 && inode->addrs[i] == 0)
                return false;
        } else if (inode->addrs[i] != 0 || inode->blklen[i] != 0) {
            return false;                 /* references past the last block */
        }
    }
    if (stored < inode->size)             /* never claim more than we store */
        return false;
    if (inode->type == JFS_TYPE_DIR && inode->size != (uint64_t)inode->nblocks * sizeof(jfs_dirblk))
        return false;
    return true;
}

int jfs_inode_load(struct jfs_fs *fs, uint32_t ino, jfs_inode *out)
{
    jfs_imap_entry *e = jfs_imap_lookup(fs, ino);
    int rc;

    if (!e || e->state != JFS_IMAP_USED)
        return JFS_ERR_NOTFOUND;

    rc = jfs_log_read_inode(fs, e->addr, out);
    if (rc != JFS_OK)
        return (rc == JFS_ERR_CORRUPT) ? JFS_ERR_CORRUPT : rc;

    if (out->ino != ino)
        return JFS_ERR_CORRUPT;           /* map and record disagree */
    if (out->flags & JFS_INODE_FREE)
        return JFS_ERR_NOTFOUND;
    if (!jfs_inode_sane(out))
        return JFS_ERR_CORRUPT;
    /* provided_mask belongs to a transaction, never to a loaded inode: a
     * metadata-only update must not look as if it re-declared data blocks. */
    out->provided_mask = 0;
    return JFS_OK;
}

int jfs_inode_init(struct jfs_fs *fs, jfs_inode *out, uint32_t ino, uint16_t type,
                   uint32_t mode)
{
    if (!out)
        return JFS_ERR_INVAL;
    memset(out, 0, sizeof(*out));
    out->ino = ino;
    out->type = type;
    out->mode = mode & JFS_PERM_BITS;
    out->uid = JFS_CURRENT_UID;
    out->gid = JFS_CURRENT_GID;
    out->flags = 0;
    out->generation = 1;
    out->nblocks = 0;
    out->linkcount = 1;
    out->self = 0;
    jfs_now(&out->ctime);
    out->mtime = out->ctime;
    (void)fs;
    return JFS_OK;
}

uint64_t jfs_inode_stored_bytes(const jfs_inode *inode)
{
    uint64_t sum = 0;
    uint32_t i;

    for (i = 0; i < JFS_MAX_FILE_BLOCKS && i < inode->nblocks; i++)
        sum += inode->blklen[i];
    return sum;
}
