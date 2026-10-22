/*
 * filesystem.c -- Layer 2, the filesystem operations.
 *
 * This file owns the mount state and implements the operations the CLI
 * exposes.  Every operation follows the same pattern:
 *
 *   1. resolve the path in the in-memory inode map (no host filesystem);
 *   2. check permissions;
 *   3. describe the intended change as a transaction (new versions of blocks,
 *      never in-place edits);
 *   4. commit (write ahead log), apply (append-only log region), end
 *      (checkpoint + journal reclamation).
 *
 * Nothing is written to the image outside those steps, which is what makes
 * the crash points meaningful.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bcache.h"
#include "checksum.h"
#include "crash.h"
#include "directory.h"
#include "disk_image.h"
#include "inode.h"
#include "journal.h"
#include "journalfs.h"
#include "permissions.h"
#include "recovery.h"
#include "storage.h"
#include "util.h"

static struct jfs_fs g_fs;

struct jfs_fs *jfs_fs_get(void)
{
    return &g_fs;
}

void jfs_crash_check_fs(const struct jfs_fs *fs, int point, const char *tag)
{
    if (fs && fs->crash_after > 0 && (int)fs->commits_done < fs->crash_after)
        return;                       /* not armed yet */
    jfs_crash_check((jfs_crash_point)point, tag);
}

/* ------------------------------------------------------------------ */
/* formatting                                                          */
/* ------------------------------------------------------------------ */

static int format_check_size(uint64_t size_bytes, char *err, size_t errsz)
{
    uint64_t blocks, log_blocks;

    if (size_bytes == 0)
        size_bytes = (uint64_t)JFS_DEFAULT_SIZE_KIB * 1024ull;
    if (size_bytes % JFS_BLOCK_SIZE != 0) {
        snprintf(err, errsz, "image size must be a multiple of %u bytes", JFS_BLOCK_SIZE);
        return JFS_ERR_INVAL;
    }
    if (size_bytes < (uint64_t)JFS_MIN_SIZE_KIB * 1024ull) {
        snprintf(err, errsz, "image size must be at least %u KiB", JFS_MIN_SIZE_KIB);
        return JFS_ERR_INVAL;
    }
    if (size_bytes > (uint64_t)JFS_MAX_SIZE_KIB * 1024ull) {
        snprintf(err, errsz, "image size must be at most %u KiB", JFS_MAX_SIZE_KIB);
        return JFS_ERR_INVAL;
    }
    blocks = size_bytes / JFS_BLOCK_SIZE;
    log_blocks = blocks - JFS_LOG_BLOCK;
    if (log_blocks / 2 < JFS_MIN_LOG_HALF_BLOCKS) {
        snprintf(err, errsz,
                 "image too small: %llu blocks leave only %llu log blocks "
                 "(need at least %u per half)",
                 (unsigned long long)blocks, (unsigned long long)log_blocks,
                 JFS_MIN_LOG_HALF_BLOCKS);
        return JFS_ERR_INVAL;
    }
    return JFS_OK;
}

static uint32_t make_image_id(void)
{
    uint32_t id = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd >= 0) {
        if (jfs_pread_full(fd, &id, sizeof(id), 0) != (ssize_t)sizeof(id))
            id = 0;
        close(fd);
    }
    if (id == 0)
        id = (uint32_t)(time(NULL) ^ (getpid() << 16));
    return id;
}

int jfs_format(const char *path, const jfs_format_opts *opts, char *err, size_t errsz)
{
    struct jfs_fs *fs = jfs_fs_get();
    jfs_format_opts local;
    uint64_t size;
    int fd, rc;
    jfs_dirblk rootblk;
    jfs_inode root;
    jfs_imap map;
    uint64_t dirblk_addr = 0, inode_addr = 0, imap_addr = 0;
    jfs_checkpoint ck;

    if (!path || !*path) {
        snprintf(err, errsz, "no image path given");
        return JFS_ERR_INVAL;
    }
    if (fs->mounted) {
        snprintf(err, errsz, "a filesystem is already mounted (%s)", fs->path);
        return JFS_ERR_MOUNTED;
    }
    local = opts ? *opts : (jfs_format_opts){0};
    size = local.size_bytes ? local.size_bytes
                            : (uint64_t)JFS_DEFAULT_SIZE_KIB * 1024ull;

    rc = format_check_size(size, err, errsz);
    if (rc != JFS_OK)
        return rc;

    jfs_crash_check(JFS_CRASH_PRE_FORMAT, path);

    fd = jfs_image_create(path, size, local.force, err, errsz);
    if (fd < 0)
        return fd;

    /* Use the global structure as scratch while formatting: it becomes live
     * only if mount succeeds later. */
    memset(fs, 0, sizeof(*fs));
    fs->fd = fd;
    jfs_strcpy(fs->path, sizeof(fs->path), path);
    jfs_layout_for_size(size, &fs->sb);
    fs->sb.image_id = make_image_id();
    fs->checkpoint_interval = 1;
    fs->group_commit_batch = 1;
    fs->journal_enabled = true;
    fs->crash_after = 0;

    memset(&fs->ckpt, 0, sizeof(fs->ckpt));
    fs->ckpt.magic      = JFS_CKPT_MAGIC;
    fs->ckpt.version    = JFS_FORMAT_VERSION;
    fs->ckpt.seq        = 1;
    fs->ckpt.next_txid  = 1;
    fs->ckpt.log_half   = 0;
    fs->ckpt.log_head   = 1;    /* block 0 of a half is the null address */
    fs->log_seq         = 0;
    fs->log_live_bytes  = 0;

    /* --- root directory -------------------------------------------------- */
    memset(&rootblk, 0, sizeof(rootblk));
    rootblk.magic = JFS_DIRBLK_MAGIC;
    rootblk.count = 0;
    rootblk.crc = 0;
    rootblk.crc = jfs_crc_struct(&rootblk, sizeof(rootblk), offsetof(jfs_dirblk, crc));
    rc = jfs_log_append(fs, JFS_LREC_DIRBLK, JFS_ROOT_INO, 0,
                        &rootblk, sizeof(rootblk), &dirblk_addr);
    if (rc != JFS_OK)
        goto out_io;

    memset(&root, 0, sizeof(root));
    jfs_inode_init(fs, &root, JFS_ROOT_INO, JFS_TYPE_DIR, JFS_DEFAULT_DIR_MODE);
    root.nblocks = 1;
    root.addrs[0] = dirblk_addr;
    root.blklen[0] = sizeof(rootblk);
    root.size = sizeof(rootblk);
    root.linkcount = 1;
    root.self = ((uint64_t)fs->ckpt.log_half << 32) | fs->ckpt.log_head;
    rc = jfs_log_append(fs, JFS_LREC_INODE, JFS_ROOT_INO, 0, &root, sizeof(root),
                        &inode_addr);
    if (rc != JFS_OK)
        goto out_io;

    memset(&map, 0, sizeof(map));
    map.magic    = JFS_IMAP_MAGIC;
    map.count    = 1;
    map.next_ino = 2;
    map.entries[0].ino   = JFS_ROOT_INO;
    map.entries[0].state = JFS_IMAP_USED;
    map.entries[0].addr  = inode_addr;
    map.crc = 0;
    map.crc = jfs_crc_struct(&map, sizeof(map), offsetof(jfs_imap, crc));
    rc = jfs_log_append(fs, JFS_LREC_IMAP, 0, 0, &map, sizeof(map), &imap_addr);
    if (rc != JFS_OK)
        goto out_io;

    fs->imap = map;
    fs->ckpt.imap_addr = imap_addr;
    fs->ckpt.log_head = fs->ckpt.log_head;
    fs->ckpt.log_seq = fs->log_seq;
    fs->ckpt.n_inodes = 1;
    fs->ckpt.live_bytes = fs->log_live_bytes;
    fs->ckpt.journal_start = 0;
    fs->ckpt.journal_used = 0;

    ck = fs->ckpt;
    ck.crc = 0;
    ck.crc = jfs_crc_struct(&ck, sizeof(ck), offsetof(jfs_checkpoint, crc));
    rc = jfs_checkpoint_write_slot(fs, &ck, 0, false);
    if (rc != JFS_OK)
        goto out_io;

    /* Everything except the superblock is on disk now.  The superblock is
     * written last on purpose: a crash half way through formatting leaves an
     * image that fails superblock validation instead of a half-built
     * filesystem that looks mountable. */
    if (fsync(fd) != 0) {
        rc = JFS_ERR_IO;
        goto out_io;
    }
    fs->sb.flags |= JFS_SB_CLEAN;
    rc = jfs_write_superblock(fs, true);
    if (rc != JFS_OK)
        goto out_io;

    close(fd);
    memset(fs, 0, sizeof(*fs));      /* formatting does not mount */
    return JFS_OK;

out_io:
    if (err && errsz)
        snprintf(err, errsz, "formatting '%s' failed: %s", path, strerror(errno));
    close(fd);
    memset(fs, 0, sizeof(*fs));
    return (rc == JFS_OK) ? JFS_ERR_IO : rc;
}

/* ------------------------------------------------------------------ */
/* mounting                                                            */
/* ------------------------------------------------------------------ */

int jfs_mount(const char *path, bool quiet, char *err, size_t errsz)
{
    struct jfs_fs *fs = jfs_fs_get();
    uint64_t image_size = 0;
    int rc;

    if (!path || !*path) {
        snprintf(err, errsz, "no image path given");
        return JFS_ERR_INVAL;
    }
    if (fs->mounted) {
        snprintf(err, errsz, "a filesystem is already mounted (%s)", fs->path);
        return JFS_ERR_MOUNTED;
    }

    memset(fs, 0, sizeof(*fs));
    fs->journal_enabled = true;
    fs->checkpoint_interval = 1;
    fs->group_commit_batch = 1;
    fs->fd = jfs_image_open(path, err, errsz);
    if (fs->fd < 0)
        return JFS_ERR_IO;
    jfs_strcpy(fs->path, sizeof(fs->path), path);

    rc = jfs_bcache_init(jfs_get_bcache(), fs->fd, JFS_BCACHE_DEFAULT_FRAMES);
    if (rc != JFS_OK) {
        snprintf(err, errsz, "failed to initialize buffer cache");
        goto fail;
    }

    rc = jfs_image_size(fs->fd, &image_size, err, errsz);
    if (rc != JFS_OK)
        goto fail;
    rc = jfs_read_superblock(fs, err, errsz);
    if (rc != JFS_OK)
        goto fail;
    rc = jfs_validate_superblock(&fs->sb, image_size, err, errsz);
    if (rc != JFS_OK)
        goto fail;

    rc = jfs_checkpoint_load_best(fs, &fs->ckpt_slot, &fs->ckpt);
    if (rc != JFS_OK) {
        snprintf(err, errsz,
                 "no valid checkpoint found: BOTH checkpoint slots are damaged "
                 "(image is not recoverable by this build)");
        goto fail;
    }

    rc = jfs_log_read_imap(fs, fs->ckpt.imap_addr, &fs->imap);
    if (rc != JFS_OK) {
        snprintf(err, errsz, "inode map is damaged (record at log address 0x%llx)",
                 (unsigned long long)fs->ckpt.imap_addr);
        goto fail;
    }
    {
        jfs_inode root;
        rc = jfs_inode_load(fs, JFS_ROOT_INO, &root);
        if (rc != JFS_OK || root.type != JFS_TYPE_DIR) {
            snprintf(err, errsz, "root inode is missing or not a directory");
            rc = JFS_ERR_CORRUPT;
            goto fail;
        }
    }

    fs->next_txid     = fs->ckpt.next_txid;
    fs->last_applied  = fs->ckpt.applied_txid;
    fs->log_seq       = fs->ckpt.log_seq;
    fs->log_live_bytes = fs->ckpt.live_bytes;
    fs->journal_lsn   = 0;
    fs->stats.checkpoint_seq = fs->ckpt.seq;
    fs->mounted = true;

    /* Re-derive the journal window by scanning; the checkpoint's copy is
     * only a hint, and a torn record must never be trusted. */
    rc = jfs_journal_rescan(fs);
    if (rc != JFS_OK) {
        snprintf(err, errsz, "cannot read the journal region");
        goto fail;
    }

    /* Mark the image dirty before recovery starts: if we are interrupted now
     * the next mount will know the shutdown was not clean. */
    (void)jfs_superblock_set_clean(fs, false, true);

    rc = jfs_recover(fs, NULL);
    if (rc != JFS_OK) {
        snprintf(err, errsz,
                 "recovery failed: the journal holds a committed transaction "
                 "that cannot be replayed (%s); refusing to mount a state that "
                 "cannot be made consistent",
                 jfs_strerror(rc));
        goto fail;
    }

    if (fs->sb.flags & JFS_SB_CLEAN) {
        /* the loader above cleared the flag in memory; this branch is only
         * reachable if the write failed, which is worth reporting */
        if (!quiet)
            fprintf(stderr, "warning: could not mark the image as in use\n");
    }
    return JFS_OK;

fail:
    jfs_bcache_destroy(jfs_get_bcache());
    close(fs->fd);
    memset(fs, 0, sizeof(*fs));
    return rc;
}

/*
 * Force a checkpoint regardless of the configured batching interval: `sync`
 * and unmount promise that everything applied so far is durable and that the
 * journal window has been reclaimed, so the deferral is switched off for the
 * duration of the call.
 */
static int checkpoint_now(struct jfs_fs *fs)
{
    uint32_t saved = fs->checkpoint_interval;
    uint32_t saved_txns = fs->txns_since_ckpt;
    int rc;

    fs->checkpoint_interval = 1;
    fs->txns_since_ckpt = 0;
    rc = jfs_finish_transaction(fs, fs->last_applied);
    fs->checkpoint_interval = saved;
    if (rc != JFS_OK)
        fs->txns_since_ckpt = saved_txns;
    return rc;
}

int jfs_unmount(void)
{
    struct jfs_fs *fs = jfs_fs_get();
    int rc;

    if (!fs->mounted)
        return JFS_ERR_NOTMOUNTED;

    if (fs->group_commit_pending > 0) {
        jfs_journal_sync(fs);
        fs->group_commit_pending = 0;
        fs->stats.group_commit_flushes++;
    }

    rc = checkpoint_now(fs);
    if (rc == JFS_OK)
        rc = jfs_bcache_flush_all(jfs_get_bcache());
    if (rc == JFS_OK)
        rc = jfs_journal_sync(fs);
    if (rc == JFS_OK) {
        char err[256];
        rc = jfs_checkpoint_write(true, err, sizeof(err));
    }
    jfs_bcache_destroy(jfs_get_bcache());
    if (fsync(fs->fd) != 0 && rc == JFS_OK)
        rc = JFS_ERR_IO;
    if (close(fs->fd) != 0 && rc == JFS_OK)
        rc = JFS_ERR_IO;
    memset(fs, 0, sizeof(*fs));
    return rc;
}

/* ------------------------------------------------------------------ */
/* path resolution                                                     */
/* ------------------------------------------------------------------ */

static int dir_lookup_ino(struct jfs_fs *fs, uint32_t dir_ino, const char *name,
                          uint32_t *ino_out, uint16_t *type_out)
{
    jfs_dirview dv;
    jfs_dirent de;
    int rc = jfs_dirview_open(fs, dir_ino, &dv);

    if (rc != JFS_OK)
        return rc;
    rc = jfs_dirview_lookup(&dv, name, &de);
    jfs_dirview_free(&dv);
    if (rc == JFS_OK) {
        if (ino_out)
            *ino_out = de.ino;
        if (type_out)
            *type_out = de.type;
    }
    return rc;
}

int jfs_resolve(const char *path, uint32_t *ino_out, uint32_t *parent_out,
                char *name_out, size_t namesz)
{
    struct jfs_fs *fs = jfs_fs_get();
    char **parts = NULL;
    size_t nparts = 0, i;
    uint32_t cur = JFS_ROOT_INO;
    int rc;

    if (ino_out)
        *ino_out = 0;
    if (parent_out)
        *parent_out = 0;
    if (name_out && namesz)
        name_out[0] = '\0';

    if (!fs->mounted)
        return JFS_ERR_NOTMOUNTED;
    if (fs->failed)
        return JFS_ERR_FAILED;
    if (!path || path[0] != '/')
        return JFS_ERR_BADPATH;

    rc = jfs_path_split(path, &parts, &nparts);
    if (rc != JFS_OK)
        return rc;

    if (nparts == 0) {                       /* "/" resolves to the root */
        if (ino_out)
            *ino_out = JFS_ROOT_INO;
        if (parent_out)
            *parent_out = JFS_ROOT_INO;
        jfs_path_free(parts, nparts);
        return JFS_OK;
    }

    /* The final component is known as soon as the path is split: record it
     * before walking, so callers that only need the name (creating a file
     * whose parent is missing, for example) still get a useful error. */
    if (name_out && namesz) {
        rc = jfs_strcpy(name_out, namesz, parts[nparts - 1]);
        if (rc != JFS_OK) {
            jfs_path_free(parts, nparts);
            return JFS_ERR_NAMETOOLONG;
        }
    }

    for (i = 0; i + 1 < nparts; i++) {
        uint32_t next = 0;
        jfs_inode inode;

        rc = dir_lookup_ino(fs, cur, parts[i], &next, NULL);
        if (rc != JFS_OK) {
            jfs_path_free(parts, nparts);
            return rc;
        }
        rc = jfs_inode_load(fs, next, &inode);
        if (rc != JFS_OK) {
            jfs_path_free(parts, nparts);
            return rc;
        }
        if (inode.type != JFS_TYPE_DIR) {
            jfs_path_free(parts, nparts);
            return JFS_ERR_NOTDIR;
        }
        rc = jfs_permission_check(&inode, JFS_X_OK, "traverse");
        if (rc != JFS_OK) {
            jfs_path_free(parts, nparts);
            return rc;
        }
        cur = next;
    }

    if (parent_out)
        *parent_out = cur;

    {
        uint32_t target = 0;
        rc = dir_lookup_ino(fs, cur, parts[nparts - 1], &target, NULL);
        if (rc == JFS_OK && ino_out)
            *ino_out = target;
    }
    jfs_path_free(parts, nparts);
    return rc;
}

int jfs_lookup(const char *path, uint32_t *ino_out, jfs_inode *out)
{
    struct jfs_fs *fs = jfs_fs_get();
    uint32_t ino = 0;
    int rc = jfs_resolve(path, &ino, NULL, NULL, 0);

    if (rc != JFS_OK)
        return rc;
    if (ino_out)
        *ino_out = ino;
    if (out)
        return jfs_inode_load(fs, ino, out);
    return JFS_OK;
}

int jfs_exists(const char *path)
{
    return jfs_lookup(path, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* transaction helpers                                                 */
/* ------------------------------------------------------------------ */

/*
 * Commit a prepared transaction and, when the commit became durable, apply
 * it.  The JFULL retry is the documented "journal is out of room" path: we
 * force a checkpoint (which reclaims the window) and try once more.
 */
static int txn_finish(jfs_txn *tx)
{
    struct jfs_fs *fs = tx->fs;
    bool apply = false;
    int rc;

    rc = jfs_txn_commit(tx, &apply);
    if (rc == JFS_ERR_JFULL || rc == JFS_ERR_NOSPACE) {
        /* Not enough room in the journal or in the log region: make the
         * applied state durable (this reclaims the journal window) and
         * compact the log when that is what the room check was complaining
         * about, then try once more.  A second refusal is reported to the
         * caller as a clean "no space" error. */
        rc = jfs_finish_transaction(fs, fs->last_applied);
        if (rc == JFS_OK && jfs_log_free_blocks(fs) < jfs_txn_log_blocks(tx))
            rc = jfs_log_compact(fs);
        if (rc == JFS_OK)
            rc = jfs_txn_commit(tx, &apply);
    }
    if (rc != JFS_OK) {
        jfs_txn_free(tx);
        return rc;
    }
    if (apply) {
        rc = jfs_txn_apply(tx);
        if (rc != JFS_OK) {
            /* The transaction is durable but only partly applied.  Stop
             * accepting work: the journal keeps the record of it and the
             * next mount replays it (redo is idempotent).  Silently
             * continuing could fill the journal with records that can never
             * be reclaimed, so this is a hard stop. */
            fs->failed = true;
            fprintf(stderr, "jfs: apply of transaction %llu failed: %s\n",
                    (unsigned long long)tx->txid, jfs_strerror(rc));
            jfs_txn_free(tx);
            return (rc == JFS_ERR_CORRUPT) ? JFS_ERR_CORRUPT : rc;
        }
    }
    return jfs_txn_end(tx);
}

/* Read the whole content of a file inode into a malloc'd buffer. */
static int read_content(struct jfs_fs *fs, const jfs_inode *inode,
                        unsigned char **out, uint64_t *size_out)
{
    uint64_t size = inode->size;
    unsigned char *buf;
    uint64_t copied = 0;
    uint32_t i;

    buf = (unsigned char *)calloc(1, size ? (size_t)size : 1);
    if (!buf)
        return JFS_ERR_NOMEM;

    for (i = 0; i < inode->nblocks && copied < size; i++) {
        unsigned char tmp[JFS_DATA_BYTES];
        uint16_t type = 0;
        uint32_t len = 0;
        uint64_t take;
        int rc;

        if (inode->addrs[i] == 0) {
            /* a hole: the file is shorter than the block map suggests */
            break;
        }
        rc = jfs_log_read(fs, inode->addrs[i], tmp, sizeof(tmp), &type, &len);
        if (rc != JFS_OK) {
            free(buf);
            return rc;
        }
        if (type != JFS_LREC_DATA || len > JFS_DATA_BYTES) {
            free(buf);
            return JFS_ERR_CORRUPT;
        }
        take = len;
        if (take > size - copied)
            take = size - copied;
        memcpy(buf + copied, tmp, (size_t)take);
        copied += take;
    }
    if (copied < size) {
        free(buf);
        return JFS_ERR_CORRUPT;
    }
    *out = buf;
    if (size_out)
        *size_out = size;
    return JFS_OK;
}

/*
 * Write `content` (newsize bytes) as the new content of `inode`, appending
 * only the blocks that actually changed.  Unchanged blocks keep pointing at
 * their previous log records, which is what keeps repeated small updates
 * cheap even though the storage is append-only.
 */
static int write_content(struct jfs_fs *fs, const jfs_inode *inode,
                         const unsigned char *content, uint64_t newsize,
                         char *err, size_t errsz)
{
    unsigned char *old = NULL;
    uint64_t oldsize = inode->size;
    uint32_t newblocks, i;
    uint32_t mask = 0;
    jfs_inode updated;
    jfs_txn *tx = NULL;
    int rc;

    if (newsize > JFS_MAX_FILE_BYTES) {
        if (err)
            snprintf(err, errsz, "file would be %llu bytes, limit is %u",
                     (unsigned long long)newsize, JFS_MAX_FILE_BYTES);
        return JFS_ERR_FILE_TOO_BIG;
    }

    rc = read_content(fs, inode, &old, NULL);
    if (rc != JFS_OK)
        return rc;

    newblocks = (uint32_t)((newsize + JFS_DATA_BYTES - 1u) / JFS_DATA_BYTES);

    rc = jfs_txn_begin(fs, &tx);
    if (rc != JFS_OK)
        goto out;

    for (i = 0; i < newblocks; i++) {
        unsigned char newblk[JFS_DATA_BYTES];
        unsigned char oldblk[JFS_DATA_BYTES];
        uint64_t off = (uint64_t)i * JFS_DATA_BYTES;
        uint32_t take = (uint32_t)((newsize - off < JFS_DATA_BYTES)
                                   ? (newsize - off) : JFS_DATA_BYTES);

        memset(newblk, 0, sizeof(newblk));
        memcpy(newblk, content + off, take);
        memset(oldblk, 0, sizeof(oldblk));
        if (off < oldsize) {
            uint32_t oldtake = (uint32_t)((oldsize - off < JFS_DATA_BYTES)
                                          ? (oldsize - off) : JFS_DATA_BYTES);
            memcpy(oldblk, old + off, oldtake);
        }
        /* Reuse the existing record only when the bytes AND the stored
         * length are unchanged: growing a file inside its last block would
         * otherwise leave the inode claiming more bytes than its data record
         * holds, which is exactly the kind of inconsistency the load time
         * validation rejects. */
        if (i < inode->nblocks && inode->blklen[i] == take &&
            memcmp(newblk, oldblk, sizeof(newblk)) == 0)
            continue;

        rc = jfs_txn_add_data(tx, inode->ino, i, newblk, take);
        if (rc != JFS_OK)
            goto out;
        mask |= (1u << i);
    }

    updated = *inode;
    updated.size = newsize;
    updated.nblocks = newblocks;
    /* Shrinking must drop the references to the blocks that are gone:
     * `updated` starts as a copy of the old inode, so addrs[]/blklen[] still
     * describe the longer file.  Leaving them behind would make the loaded
     * inode claim blocks past its own last block, which the load time
     * validation refuses (and which would let a later reader touch blocks
     * that have already been released). */
    for (i = newblocks; i < JFS_MAX_FILE_BLOCKS; i++) {
        updated.addrs[i] = 0;
        updated.blklen[i] = 0;
    }
    updated.provided_mask = mask;
    updated.generation++;
    jfs_now(&updated.mtime);
    rc = jfs_txn_add_inode(tx, &updated);
    if (rc != JFS_OK)
        goto out;

    rc = txn_finish(tx);
    tx = NULL;

out:
    free(old);
    if (tx)
        jfs_txn_free(tx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* file operations                                                     */
/* ------------------------------------------------------------------ */

int jfs_create(const char *path, uint32_t mode)
{
    struct jfs_fs *fs = jfs_fs_get();
    uint32_t parent = 0, existing = 0, newino;
    char name[JFS_NAME_MAX + 1];
    jfs_inode pdir, ninode;
    jfs_dirview dv;
    jfs_dirent de;
    jfs_txn *tx = NULL;
    char err[128];
    int rc;

    rc = jfs_resolve(path, &existing, &parent, name, sizeof(name));
    if (rc == JFS_OK)
        return JFS_ERR_EXISTS;
    if (rc != JFS_ERR_NOTFOUND)
        return rc;
    if (!jfs_valid_name(name))
        return (strlen(name) > JFS_NAME_MAX) ? JFS_ERR_NAMETOOLONG : JFS_ERR_INVAL;

    rc = jfs_inode_load(fs, parent, &pdir);
    if (rc != JFS_OK)
        return rc;
    if (pdir.type != JFS_TYPE_DIR)
        return JFS_ERR_NOTDIR;
    rc = jfs_permission_check(&pdir, JFS_W_OK | JFS_X_OK, "create");
    if (rc != JFS_OK)
        return rc;

    newino = jfs_inode_alloc_number(fs, err, sizeof(err));
    if (newino == 0)
        return JFS_ERR_NOINODES;

    rc = jfs_inode_init(fs, &ninode, newino, JFS_TYPE_FILE,
                        mode ? mode : JFS_DEFAULT_FILE_MODE);
    if (rc != JFS_OK)
        return rc;

    rc = jfs_dirview_open(fs, parent, &dv);
    if (rc != JFS_OK)
        return rc;
    memset(&de, 0, sizeof(de));
    jfs_strcpy(de.name, sizeof(de.name), name);
    de.ino = newino;
    de.type = JFS_TYPE_FILE;
    de.flags = JFS_DE_VALID;
    rc = jfs_dirview_insert(&dv, &de);
    if (rc != JFS_OK) {
        jfs_dirview_free(&dv);
        return rc;
    }

    rc = jfs_txn_begin(fs, &tx);
    if (rc != JFS_OK) {
        jfs_dirview_free(&dv);
        return rc;
    }
    rc = jfs_txn_add_inode(tx, &ninode);
    if (rc == JFS_OK)
        rc = jfs_dirview_to_txn(fs, tx, &dv);
    if (rc != JFS_OK) {
        jfs_txn_free(tx);
        jfs_dirview_free(&dv);
        return rc;
    }
    rc = txn_finish(tx);
    jfs_dirview_free(&dv);
    return rc;
}

int jfs_mkdir(const char *path, uint32_t mode)
{
    struct jfs_fs *fs = jfs_fs_get();
    uint32_t parent = 0, existing = 0, newino;
    char name[JFS_NAME_MAX + 1];
    jfs_inode pdir, ninode;
    jfs_dirview dv;
    jfs_dirblk empty;
    jfs_dirent de;
    jfs_txn *tx = NULL;
    char err[128];
    int rc;

    rc = jfs_resolve(path, &existing, &parent, name, sizeof(name));
    if (rc == JFS_OK)
        return JFS_ERR_EXISTS;
    if (rc != JFS_ERR_NOTFOUND)
        return rc;
    if (!jfs_valid_name(name))
        return (strlen(name) > JFS_NAME_MAX) ? JFS_ERR_NAMETOOLONG : JFS_ERR_INVAL;

    rc = jfs_inode_load(fs, parent, &pdir);
    if (rc != JFS_OK)
        return rc;
    rc = jfs_permission_check(&pdir, JFS_W_OK | JFS_X_OK, "mkdir");
    if (rc != JFS_OK)
        return rc;

    newino = jfs_inode_alloc_number(fs, err, sizeof(err));
    if (newino == 0)
        return JFS_ERR_NOINODES;

    rc = jfs_inode_init(fs, &ninode, newino, JFS_TYPE_DIR,
                        mode ? mode : JFS_DEFAULT_DIR_MODE);
    if (rc != JFS_OK)
        return rc;

    memset(&empty, 0, sizeof(empty));
    empty.magic = JFS_DIRBLK_MAGIC;
    empty.crc = jfs_crc_struct(&empty, sizeof(empty), offsetof(jfs_dirblk, crc));
    ninode.nblocks = 1;
    ninode.size = sizeof(empty);
    ninode.provided_mask = 1u;
    ninode.linkcount = 1;

    rc = jfs_dirview_open(fs, parent, &dv);
    if (rc != JFS_OK)
        return rc;
    memset(&de, 0, sizeof(de));
    jfs_strcpy(de.name, sizeof(de.name), name);
    de.ino = newino;
    de.type = JFS_TYPE_DIR;
    de.flags = JFS_DE_VALID;
    rc = jfs_dirview_insert(&dv, &de);
    if (rc != JFS_OK) {
        jfs_dirview_free(&dv);
        return rc;
    }

    rc = jfs_txn_begin(fs, &tx);
    if (rc != JFS_OK) {
        jfs_dirview_free(&dv);
        return rc;
    }
    rc = jfs_txn_add_dirblk(tx, newino, 0, &empty);
    if (rc == JFS_OK)
        rc = jfs_txn_add_inode(tx, &ninode);
    if (rc == JFS_OK)
        rc = jfs_dirview_to_txn(fs, tx, &dv);
    if (rc != JFS_OK) {
        jfs_txn_free(tx);
        jfs_dirview_free(&dv);
        return rc;
    }
    rc = txn_finish(tx);
    jfs_dirview_free(&dv);
    return rc;
}

int jfs_read(const char *path, jfs_buf *out)
{
    return jfs_read_range(path, 0, 0, out);
}

int jfs_read_range(const char *path, uint64_t off, uint64_t len, jfs_buf *out)
{
    struct jfs_fs *fs = jfs_fs_get();
    jfs_inode inode;
    unsigned char *content = NULL;
    uint64_t size = 0;
    int rc;

    if (!out)
        return JFS_ERR_INVAL;
    rc = jfs_lookup(path, NULL, &inode);
    if (rc != JFS_OK)
        return rc;
    if (inode.type == JFS_TYPE_DIR)
        return JFS_ERR_ISDIR;
    rc = jfs_permission_check(&inode, JFS_R_OK, "read");
    if (rc != JFS_OK)
        return rc;

    rc = read_content(fs, &inode, &content, &size);
    if (rc != JFS_OK)
        return rc;

    if (off > size)
        off = size;
    if (len == 0 || off + len > size)
        len = size - off;

    rc = jfs_buf_append(out, content + off, (size_t)len);
    free(content);
    return rc;
}

int jfs_write(const char *path, const void *data, size_t len, uint64_t off,
              bool append, bool do_truncate)
{
    struct jfs_fs *fs = jfs_fs_get();
    jfs_inode inode;
    unsigned char *old = NULL, *content = NULL;
    uint64_t oldsize = 0, newsize;
    int rc;

    rc = jfs_lookup(path, NULL, &inode);
    if (rc != JFS_OK)
        return rc;
    if (inode.type == JFS_TYPE_DIR)
        return JFS_ERR_ISDIR;
    rc = jfs_permission_check(&inode, JFS_W_OK, "write");
    if (rc != JFS_OK)
        return rc;

    rc = read_content(fs, &inode, &old, &oldsize);
    if (rc != JFS_OK)
        return rc;

    if (append)
        off = oldsize;
    newsize = do_truncate ? off + len : (oldsize > off + len ? oldsize : off + len);
    if (newsize > JFS_MAX_FILE_BYTES) {
        free(old);
        return JFS_ERR_FILE_TOO_BIG;
    }

    content = (unsigned char *)calloc(1, newsize ? (size_t)newsize : 1);
    if (!content) {
        free(old);
        return JFS_ERR_NOMEM;
    }
    if (!do_truncate && oldsize) {
        uint64_t keep = oldsize < newsize ? oldsize : newsize;
        memcpy(content, old, (size_t)keep);
    }
    if (len)
        memcpy(content + off, data, len);
    free(old);

    rc = write_content(fs, &inode, content, newsize, NULL, 0);
    free(content);
    return rc;
}

int jfs_truncate(const char *path, uint64_t size)
{
    struct jfs_fs *fs = jfs_fs_get();
    jfs_inode inode;
    unsigned char *old = NULL, *content = NULL;
    uint64_t oldsize = 0;
    int rc;

    rc = jfs_lookup(path, NULL, &inode);
    if (rc != JFS_OK)
        return rc;
    if (inode.type == JFS_TYPE_DIR)
        return JFS_ERR_ISDIR;
    rc = jfs_permission_check(&inode, JFS_W_OK, "truncate");
    if (rc != JFS_OK)
        return rc;
    if (size > JFS_MAX_FILE_BYTES)
        return JFS_ERR_FILE_TOO_BIG;

    rc = read_content(fs, &inode, &old, &oldsize);
    if (rc != JFS_OK)
        return rc;

    content = (unsigned char *)calloc(1, size ? (size_t)size : 1);
    if (!content) {
        free(old);
        return JFS_ERR_NOMEM;
    }
    if (size && oldsize) {
        uint64_t keep = oldsize < size ? oldsize : size;
        memcpy(content, old, (size_t)keep);
    }
    free(old);

    rc = write_content(fs, &inode, content, size, NULL, 0);
    free(content);
    return rc;
}

int jfs_readdir(const char *path, void (*emit)(void *, const char *, const jfs_dirent *),
                void *ud)
{
    struct jfs_fs *fs = jfs_fs_get();
    jfs_inode inode;
    jfs_dirview dv;
    uint32_t i, k;
    int rc;

    rc = jfs_lookup(path, NULL, &inode);
    if (rc != JFS_OK)
        return rc;
    if (inode.type != JFS_TYPE_DIR)
        return JFS_ERR_NOTDIR;
    rc = jfs_permission_check(&inode, JFS_R_OK, "list");
    if (rc != JFS_OK)
        return rc;

    rc = jfs_dirview_open(fs, inode.ino, &dv);
    if (rc != JFS_OK)
        return rc;
    for (i = 0; i < dv.nblocks; i++) {
        for (k = 0; k < dv.blk[i].count; k++) {
            if (dv.blk[i].ents[k].flags & JFS_DE_VALID)
                emit(ud, dv.blk[i].ents[k].name, &dv.blk[i].ents[k]);
        }
    }
    jfs_dirview_free(&dv);
    return JFS_OK;
}

/* Recursive part of jfs_rm() below. */
static int remove_path(struct jfs_fs *fs, const char *path, bool recursive);

static int remove_leaf(struct jfs_fs *fs, uint32_t parent, const char *name,
                       const jfs_inode *target)
{
    jfs_dirview dv;
    jfs_dirent removed;
    jfs_inode pdir;
    jfs_inode freeme;
    jfs_txn *tx = NULL;
    int rc;

    rc = jfs_inode_load(fs, parent, &pdir);
    if (rc != JFS_OK)
        return rc;
    rc = jfs_permission_check(&pdir, JFS_W_OK | JFS_X_OK, "unlink");
    if (rc != JFS_OK)
        return rc;

    rc = jfs_dirview_open(fs, parent, &dv);
    if (rc != JFS_OK)
        return rc;
    rc = jfs_dirview_remove(&dv, name, &removed);
    if (rc != JFS_OK) {
        jfs_dirview_free(&dv);
        return rc;
    }

    freeme = *target;
    freeme.provided_mask = 0;
    freeme.flags |= JFS_INODE_FREE;
    freeme.generation++;
    jfs_now(&freeme.mtime);

    rc = jfs_txn_begin(fs, &tx);
    if (rc != JFS_OK) {
        jfs_dirview_free(&dv);
        return rc;
    }
    rc = jfs_dirview_to_txn(fs, tx, &dv);
    if (rc == JFS_OK)
        rc = jfs_txn_add_inode(tx, &freeme);
    if (rc != JFS_OK) {
        jfs_txn_free(tx);
        jfs_dirview_free(&dv);
        return rc;
    }
    rc = txn_finish(tx);
    jfs_dirview_free(&dv);
    return rc;
}

static int remove_path(struct jfs_fs *fs, const char *path, bool recursive)
{
    uint32_t ino = 0, parent = 0;
    char name[JFS_NAME_MAX + 1];
    jfs_inode inode;
    int rc;

    rc = jfs_resolve(path, &ino, &parent, name, sizeof(name));
    if (rc != JFS_OK)
        return rc;
    if (name[0] == '\0')
        return JFS_ERR_INVAL;                 /* the root cannot be removed */
    rc = jfs_inode_load(fs, ino, &inode);
    if (rc != JFS_OK)
        return rc;

    if (inode.type == JFS_TYPE_DIR) {
        jfs_dirview dv;

        if (!recursive)
            return JFS_ERR_ISDIR;
        rc = jfs_dirview_open(fs, ino, &dv);
        if (rc != JFS_OK)
            return rc;
        while (!jfs_dirview_isempty(&dv)) {
            char child[1024];
            const char *cname = NULL;

            for (uint32_t i = 0; i < dv.nblocks && !cname; i++) {
                if (dv.blk[i].count > 0)
                    cname = dv.blk[i].ents[0].name;
            }
            if (!cname) {
                jfs_dirview_free(&dv);
                return JFS_ERR_CORRUPT;
            }
            snprintf(child, sizeof(child), "%s%s%s", path,
                     (path[strlen(path) - 1] == '/') ? "" : "/", cname);
            jfs_dirview_free(&dv);
            rc = remove_path(fs, child, true);
            if (rc != JFS_OK)
                return rc;
            rc = jfs_dirview_open(fs, ino, &dv);
            if (rc != JFS_OK)
                return rc;
        }
        jfs_dirview_free(&dv);
    }
    return remove_leaf(fs, parent, name, &inode);
}

int jfs_rm(const char *path, bool recursive)
{
    return remove_path(jfs_fs_get(), path, recursive);
}

int jfs_rmdir(const char *path)
{
    struct jfs_fs *fs = jfs_fs_get();
    uint32_t ino = 0, parent = 0;
    char name[JFS_NAME_MAX + 1];
    jfs_inode inode;
    jfs_dirview dv;
    int rc;

    rc = jfs_resolve(path, &ino, &parent, name, sizeof(name));
    if (rc != JFS_OK)
        return rc;
    if (name[0] == '\0')
        return JFS_ERR_INVAL;
    rc = jfs_inode_load(fs, ino, &inode);
    if (rc != JFS_OK)
        return rc;
    if (inode.type != JFS_TYPE_DIR)
        return JFS_ERR_NOTDIR;

    rc = jfs_dirview_open(fs, ino, &dv);
    if (rc != JFS_OK)
        return rc;
    if (!jfs_dirview_isempty(&dv)) {
        jfs_dirview_free(&dv);
        return JFS_ERR_NOTEMPTY;
    }
    jfs_dirview_free(&dv);
    return remove_leaf(fs, parent, name, &inode);
}

int jfs_rename(const char *from, const char *to)
{
    struct jfs_fs *fs = jfs_fs_get();
    uint32_t fino = 0, fparent = 0, tparent = 0, texists = 0;
    char fname[JFS_NAME_MAX + 1], tname[JFS_NAME_MAX + 1];
    jfs_inode finode, tpdir;
    jfs_dirview sdv, ddv;
    bool have_sdv = false, have_ddv = false;
    jfs_txn *tx = NULL;
    int rc;

    if (!from || !to)
        return JFS_ERR_INVAL;
    rc = jfs_resolve(from, &fino, &fparent, fname, sizeof(fname));
    if (rc != JFS_OK)
        return rc;
    if (fname[0] == '\0')
        return JFS_ERR_INVAL;
    rc = jfs_resolve(to, &texists, &tparent, tname, sizeof(tname));
    if (rc == JFS_OK)
        return JFS_ERR_EXISTS;                /* no silent overwrite */
    if (rc != JFS_ERR_NOTFOUND)
        return rc;
    if (!jfs_valid_name(tname))
        return (strlen(tname) > JFS_NAME_MAX) ? JFS_ERR_NAMETOOLONG : JFS_ERR_INVAL;
    /* A directory must not be moved into itself. */
    if (strncmp(to, from, strlen(from)) == 0 && to[strlen(from)] == '/')
        return JFS_ERR_INVAL;

    rc = jfs_inode_load(fs, fino, &finode);
    if (rc != JFS_OK)
        return rc;

    jfs_inode fpdir;
    rc = jfs_inode_load(fs, fparent, &fpdir);
    if (rc != JFS_OK)
        return rc;
    rc = jfs_permission_check(&fpdir, JFS_W_OK | JFS_X_OK, "rename");
    if (rc != JFS_OK)
        return rc;

    rc = jfs_inode_load(fs, tparent, &tpdir);
    if (rc != JFS_OK)
        return rc;
    rc = jfs_permission_check(&tpdir, JFS_W_OK | JFS_X_OK, "rename");
    if (rc != JFS_OK)
        return rc;

    rc = jfs_txn_begin(fs, &tx);
    if (rc != JFS_OK)
        return rc;

    if (fparent == tparent) {
        /* one directory, one new block version */
        rc = jfs_dirview_open(fs, fparent, &sdv);
        have_sdv = true;
        if (rc == JFS_OK)
            rc = jfs_dirview_remove(&sdv, fname, NULL);
        if (rc == JFS_OK) {
            jfs_dirent de;
            memset(&de, 0, sizeof(de));
            jfs_strcpy(de.name, sizeof(de.name), tname);
            de.ino = fino;
            de.type = finode.type;
            de.flags = JFS_DE_VALID;
            rc = jfs_dirview_insert(&sdv, &de);
        }
        if (rc == JFS_OK)
            rc = jfs_dirview_to_txn(fs, tx, &sdv);
    } else {
        jfs_dirent de;

        rc = jfs_dirview_open(fs, fparent, &sdv);
        have_sdv = true;
        if (rc == JFS_OK)
            rc = jfs_dirview_remove(&sdv, fname, NULL);
        if (rc == JFS_OK)
            rc = jfs_dirview_to_txn(fs, tx, &sdv);
        if (rc == JFS_OK) {
            rc = jfs_dirview_open(fs, tparent, &ddv);
            have_ddv = true;
        }
        if (rc == JFS_OK) {
            memset(&de, 0, sizeof(de));
            jfs_strcpy(de.name, sizeof(de.name), tname);
            de.ino = fino;
            de.type = finode.type;
            de.flags = JFS_DE_VALID;
            rc = jfs_dirview_insert(&ddv, &de);
        }
        if (rc == JFS_OK)
            rc = jfs_dirview_to_txn(fs, tx, &ddv);
    }
    if (rc != JFS_OK) {
        if (have_sdv)
            jfs_dirview_free(&sdv);
        if (have_ddv)
            jfs_dirview_free(&ddv);
        jfs_txn_free(tx);
        return rc;
    }
    rc = txn_finish(tx);
    if (have_sdv)
        jfs_dirview_free(&sdv);
    if (have_ddv)
        jfs_dirview_free(&ddv);
    return rc;
}

int jfs_stat(const char *path, jfs_inode *out)
{
    if (!out)
        return JFS_ERR_INVAL;
    return jfs_lookup(path, NULL, out);
}

int jfs_chmod(const char *path, uint32_t mode)
{
    struct jfs_fs *fs = jfs_fs_get();
    jfs_inode inode, updated;
    jfs_txn *tx = NULL;
    int rc;

    rc = jfs_lookup(path, NULL, &inode);
    if (rc != JFS_OK)
        return rc;

    updated = inode;
    updated.mode = mode & JFS_PERM_BITS;
    updated.provided_mask = 0;
    updated.generation++;
    jfs_now(&updated.mtime);

    rc = jfs_txn_begin(fs, &tx);
    if (rc != JFS_OK)
        return rc;
    rc = jfs_txn_add_inode(tx, &updated);
    if (rc != JFS_OK) {
        jfs_txn_free(tx);
        return rc;
    }
    return txn_finish(tx);
}

/* ------------------------------------------------------------------ */
/* housekeeping                                                        */
/* ------------------------------------------------------------------ */

int jfs_df(uint64_t *total_bytes, uint64_t *used_bytes,
           uint64_t *used_inodes, uint64_t *max_inodes, uint64_t *log_free)
{
    struct jfs_fs *fs = jfs_fs_get();

    if (!fs->mounted)
        return JFS_ERR_NOTMOUNTED;
    if (total_bytes)
        *total_bytes = fs->sb.total_blocks * JFS_BLOCK_SIZE;
    if (used_bytes)
        *used_bytes = fs->log_live_bytes +
                      fs->ckpt.journal_used * JFS_BLOCK_SIZE +
                      (1u + JFS_CHECKPOINT_BLOCKS) * JFS_BLOCK_SIZE;
    if (used_inodes)
        *used_inodes = fs->imap.count;
    if (max_inodes)
        *max_inodes = fs->sb.max_inodes;
    if (log_free)
        *log_free = jfs_log_free_blocks(fs);
    return JFS_OK;
}

int jfs_sync(void)
{
    struct jfs_fs *fs = jfs_fs_get();

    if (!fs->mounted)
        return JFS_ERR_NOTMOUNTED;
    int rc = jfs_bcache_flush_all(jfs_get_bcache());
    if (rc != JFS_OK)
        return rc;
    return checkpoint_now(fs);
}

int jfs_checkpoint_load(struct jfs_fs *fs, jfs_checkpoint *out)
{
    int slot = 0;
    return jfs_checkpoint_load_best(fs, &slot, out);
}
