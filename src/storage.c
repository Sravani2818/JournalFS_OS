/*
 * storage.c -- Layer 5, the log-structured storage engine.
 *
 * Records are appended to the active half of the log region.  A log address
 * is 64 bits: the high 32 bits select the half, the low 32 bits are the
 * block offset inside that half.  Nothing is ever updated in place, which is
 * what lets recovery redo an operation without worrying about what is
 * already on disk: redoing a write simply appends a newer version of the
 * record and the newest version wins because the imap/inode reference it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bcache.h"
#include "checksum.h"
#include "crash.h"
#include "journalfs.h"
#include "storage.h"
#include "util.h"

static uint64_t half_blocks(const struct jfs_fs *fs)
{
    return fs->sb.log_blocks / 2u;
}

uint64_t jfs_log_off(const struct jfs_fs *fs, uint64_t addr)
{
    uint64_t half = (addr >> 32) & 0xFFFFFFFFu;
    uint64_t off = addr & 0xFFFFFFFFu;

    return (fs->sb.log_block + half * half_blocks(fs) + off) * JFS_BLOCK_SIZE;
}

uint64_t jfs_log_free_blocks(const struct jfs_fs *fs)
{
    uint64_t hb = half_blocks(fs);
    return (fs->ckpt.log_head >= hb) ? 0 : hb - fs->ckpt.log_head;
}

uint64_t jfs_log_half_blocks(const struct jfs_fs *fs)
{
    return half_blocks(fs);
}

uint64_t jfs_log_live_bytes(const struct jfs_fs *fs)
{
    return fs->log_live_bytes;
}

/* ------------------------------------------------------------------ */
/* append                                                              */
/* ------------------------------------------------------------------ */

static int log_write_record(struct jfs_fs *fs, uint64_t half, uint64_t block_off,
                            uint16_t type, uint32_t obj_ino, uint32_t block_index,
                            const void *payload, uint32_t payload_len,
                            uint64_t *addr_out)
{
    jfs_log_rec_hdr hdr;
    uint64_t hb = half_blocks(fs);
    uint64_t total = sizeof(hdr) + payload_len;
    uint64_t nblocks = (total + JFS_BLOCK_SIZE - 1u) / JFS_BLOCK_SIZE;
    unsigned char *stage;
    size_t stage_bytes;
    int rc;

    if (block_off == 0 || block_off + nblocks > hb)
        return JFS_ERR_NOSPACE;   /* block 0 of a half is the null address */

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic       = JFS_LREC_MAGIC;
    hdr.version     = JFS_FORMAT_VERSION;
    hdr.type        = type;
    hdr.addr        = (half << 32) | block_off;
    hdr.seq         = ++fs->log_seq;
    hdr.obj_ino     = obj_ino;
    hdr.block_index = block_index;
    hdr.payload_len = payload_len;
    hdr.payload_crc = payload_len ? jfs_crc32c(payload, payload_len) : 0u;
    hdr.image_id    = fs->sb.image_id;
    hdr.hdr_crc     = 0;
    hdr.hdr_crc     = jfs_crc_struct(&hdr, sizeof(hdr), offsetof(jfs_log_rec_hdr, hdr_crc));

    /* Stage the record in one contiguous, zero padded buffer: the tail of
     * the last block is written as zeros so a torn write cannot leave stale
     * bytes from an older record behind. */
    stage_bytes = (size_t)nblocks * JFS_BLOCK_SIZE;
    stage = (unsigned char *)calloc(1, stage_bytes);
    if (!stage)
        return JFS_ERR_NOMEM;
    memcpy(stage, &hdr, sizeof(hdr));
    if (payload_len)
        memcpy(stage + sizeof(hdr), payload, payload_len);

    uint64_t start_byte_off = jfs_log_off(fs, (half << 32) | block_off);
    uint64_t start_block_no = start_byte_off / JFS_BLOCK_SIZE;
    jfs_bcache *bc = jfs_get_bcache();

    if (bc->enabled && bc->fd == fs->fd) {
        for (uint64_t bi = 0; bi < nblocks; bi++) {
            rc = jfs_bcache_write_block(bc, start_block_no + bi, stage + (bi * JFS_BLOCK_SIZE));
            if (rc != JFS_OK) {
                free(stage);
                return rc;
            }
        }
        /* Ensure log blocks reach backing store so crash points after write see durable records */
        (void)jfs_bcache_flush_all(bc);
    } else {
        ssize_t n = jfs_pwrite_full(fs->fd, stage, stage_bytes, (off_t)start_byte_off);
        if (n < 0 || (size_t)n != stage_bytes) {
            free(stage);
            return JFS_ERR_IO;
        }
    }
    free(stage);

    fs->stats.bytes_written += stage_bytes;
    if (addr_out)
        *addr_out = (half << 32) | block_off;
    return JFS_OK;
}

int jfs_log_append(struct jfs_fs *fs, uint16_t type, uint32_t obj_ino,
                   uint32_t block_index, const void *payload, uint32_t payload_len,
                   uint64_t *addr_out)
{
    int rc;

    rc = log_write_record(fs, fs->ckpt.log_half, fs->ckpt.log_head, type,
                          obj_ino, block_index, payload, payload_len, addr_out);
    if (rc != JFS_OK)
        return rc;

    {
        uint64_t total = sizeof(jfs_log_rec_hdr) + payload_len;
        uint64_t nblocks = (total + JFS_BLOCK_SIZE - 1u) / JFS_BLOCK_SIZE;
        fs->ckpt.log_head += nblocks;
        fs->ckpt.log_seq = fs->log_seq;
        fs->log_live_bytes += nblocks * JFS_BLOCK_SIZE;
    }
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* read                                                                */
/* ------------------------------------------------------------------ */

int jfs_log_read(struct jfs_fs *fs, uint64_t addr, void *buf, size_t bufsz,
                 uint16_t *type, uint32_t *len)
{
    jfs_log_rec_hdr hdr;
    uint64_t off = jfs_log_off(fs, addr);
    uint64_t block_no = off / JFS_BLOCK_SIZE;
    size_t in_blk_off = off % JFS_BLOCK_SIZE;
    jfs_bcache *bc = jfs_get_bcache();
    uint8_t blk[JFS_BLOCK_SIZE];
    int rc;
    if (bc->enabled && bc->fd == fs->fd) {
        rc = jfs_bcache_read_block(bc, block_no, blk);
        if (rc != JFS_OK)
            return rc;
    } else {
        ssize_t n = jfs_pread_full(fs->fd, blk, JFS_BLOCK_SIZE, (off_t)block_no * JFS_BLOCK_SIZE);
        if (n < 0) return JFS_ERR_IO;
    }

    if (in_blk_off + sizeof(hdr) <= JFS_BLOCK_SIZE) {
        memcpy(&hdr, blk + in_blk_off, sizeof(hdr));
    } else {
        /* Straddles block boundary (rare/unlikely since records are block-aligned) */
        ssize_t n = jfs_pread_full(fs->fd, &hdr, sizeof(hdr), (off_t)off);
        if (n < 0) return JFS_ERR_IO;
        if ((size_t)n != sizeof(hdr)) return JFS_ERR_TRUNCATED;
    }

    if (hdr.magic != JFS_LREC_MAGIC)
        return JFS_ERR_CORRUPT;
    if (hdr.version != JFS_FORMAT_VERSION)
        return JFS_ERR_CORRUPT;
    if (hdr.image_id != fs->sb.image_id)
        return JFS_ERR_CORRUPT;   /* record from a different image */
    if (hdr.addr != addr)
        return JFS_ERR_CORRUPT;   /* record is not where it claims to be */
    if (!jfs_log_rec_check(&hdr))
        return JFS_ERR_CORRUPT;
    if (hdr.payload_len > (uint32_t)(half_blocks(fs) * JFS_BLOCK_SIZE))
        return JFS_ERR_CORRUPT;

    if (buf) {
        if (hdr.payload_len > bufsz)
            return JFS_ERR_INVAL;   /* caller buffer too small */

        uint64_t p_off = off + sizeof(hdr);
        uint64_t p_blk = p_off / JFS_BLOCK_SIZE;
        size_t p_in = p_off % JFS_BLOCK_SIZE;
        uint32_t remaining = hdr.payload_len;
        uint8_t *dst = (uint8_t *)buf;

        while (remaining > 0) {
            if (bc->enabled && bc->fd == fs->fd) {
                rc = jfs_bcache_read_block(bc, p_blk, blk);
                if (rc != JFS_OK)
                    return rc;
            } else {
                ssize_t n = jfs_pread_full(fs->fd, blk, JFS_BLOCK_SIZE, (off_t)p_blk * JFS_BLOCK_SIZE);
                if (n < 0) return JFS_ERR_IO;
            }
            size_t take = JFS_BLOCK_SIZE - p_in;
            if (take > remaining)
                take = remaining;
            memcpy(dst, blk + p_in, take);
            dst += take;
            remaining -= take;
            p_blk++;
            p_in = 0;
        }

        if (hdr.payload_len && jfs_crc32c(buf, hdr.payload_len) != hdr.payload_crc)
            return JFS_ERR_CORRUPT;
    }
    if (type)
        *type = hdr.type;
    if (len)
        *len = hdr.payload_len;
    return JFS_OK;
}

int jfs_log_read_inode(struct jfs_fs *fs, uint64_t addr, jfs_inode *out)
{
    uint16_t type = 0;
    uint32_t len = 0;
    int rc = jfs_log_read(fs, addr, out, sizeof(*out), &type, &len);

    if (rc != JFS_OK)
        return rc;
    if (type != JFS_LREC_INODE || len != sizeof(jfs_inode))
        return JFS_ERR_CORRUPT;
    return JFS_OK;
}

int jfs_log_read_imap(struct jfs_fs *fs, uint64_t addr, jfs_imap *out)
{
    uint16_t type = 0;
    uint32_t len = 0;
    int rc = jfs_log_read(fs, addr, out, sizeof(*out), &type, &len);

    if (rc != JFS_OK)
        return rc;
    if (type != JFS_LREC_IMAP || len != sizeof(jfs_imap))
        return JFS_ERR_CORRUPT;
    if (out->magic != JFS_IMAP_MAGIC)
        return JFS_ERR_CORRUPT;
    if (!jfs_imap_verify(out))
        return JFS_ERR_CORRUPT;
    return JFS_OK;
}

int jfs_log_read_dirblk(struct jfs_fs *fs, uint64_t addr, jfs_dirblk *out)
{
    uint16_t type = 0;
    uint32_t len = 0;
    int rc = jfs_log_read(fs, addr, out, sizeof(*out), &type, &len);

    if (rc != JFS_OK)
        return rc;
    if (type != JFS_LREC_DIRBLK || len != sizeof(jfs_dirblk))
        return JFS_ERR_CORRUPT;
    if (out->magic != JFS_DIRBLK_MAGIC)
        return JFS_ERR_CORRUPT;
    if (out->count > JFS_DIRENTS_PER_BLOCK)
        return JFS_ERR_CORRUPT;
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* compaction                                                          */
/* ------------------------------------------------------------------ */

/*
 * Compaction is a full copy of the live data set:
 *
 *   1. walk the imap and copy every live inode, its data blocks and its
 *      directory blocks into the *inactive* half; leave the newest imap
 *      record for last, so the new half only becomes self consistent
 *      (imap present) as the final appendable step;
 *   2. write a new checkpoint that names the new half, its head, and the
 *      address of the new imap record, then fsync;
 *   3. only after the checkpoint is durable does the new half become the
 *      active half and the old half get reclaimed.
 *
 * A crash before step 2 leaves the previous half fully intact and still
 * referenced by the previous checkpoint: the copy is simply wasted space.
 */
int jfs_log_compact(struct jfs_fs *fs)
{
    uint64_t new_half = (fs->ckpt.log_half == 0) ? 1u : 0u;
    uint64_t hb = half_blocks(fs);
    uint64_t off = 1;             /* block 0 stays reserved (null address) */
    uint64_t *new_inode_addr;
    uint64_t *new_data_addr;      /* max inodes * max file blocks */
    uint64_t live = 0;
    uint32_t i, j;
    int rc = JFS_OK;
    jfs_imap newmap;

    new_inode_addr = (uint64_t *)calloc(JFS_MAX_INODES, sizeof(uint64_t));
    new_data_addr  = (uint64_t *)calloc((size_t)JFS_MAX_INODES * JFS_MAX_FILE_BLOCKS,
                                        sizeof(uint64_t));
    if (!new_inode_addr || !new_data_addr) {
        free(new_inode_addr);
        free(new_data_addr);
        return JFS_ERR_NOMEM;
    }
    memcpy(&newmap, &fs->imap, sizeof(newmap));

    jfs_crash_check(JFS_CRASH_COMPACT_MID, "compaction started");

    for (i = 0; i < JFS_MAX_INODES && rc == JFS_OK; i++) {
        jfs_imap_entry *e = &fs->imap.entries[i];
        jfs_inode inode;

        if (e->state != JFS_IMAP_USED)
            continue;
        rc = jfs_log_read_inode(fs, e->addr, &inode);
        if (rc != JFS_OK)
            break;
        if (inode.flags & JFS_INODE_FREE)
            continue;

        /* copy data / directory blocks */
        for (j = 0; j < inode.nblocks && j < JFS_MAX_FILE_BLOCKS; j++) {
            unsigned char buf[JFS_DATA_BYTES];
            uint16_t type = 0;
            uint32_t len = 0;
            uint64_t naddr = 0;
            uint64_t need;

            if (inode.addrs[j] == 0)
                continue;   /* sparse/absent block */
            rc = jfs_log_read(fs, inode.addrs[j], buf, sizeof(buf), &type, &len);
            if (rc != JFS_OK)
                break;
            need = (sizeof(jfs_log_rec_hdr) + len + JFS_BLOCK_SIZE - 1u) / JFS_BLOCK_SIZE;
            if (off + need > hb) {
                rc = JFS_ERR_NOSPACE;
                break;
            }
            rc = log_write_record(fs, new_half, off, type, inode.ino, j,
                                  buf, len, &naddr);
            if (rc != JFS_OK)
                break;
            new_data_addr[(size_t)i * JFS_MAX_FILE_BLOCKS + j] = naddr;
            off += need;
            live += need * JFS_BLOCK_SIZE;
        }
        if (rc != JFS_OK)
            break;

        /* rewrite the inode pointing at the copies */
        for (j = 0; j < JFS_MAX_FILE_BLOCKS; j++) {
            if (new_data_addr[(size_t)i * JFS_MAX_FILE_BLOCKS + j]) {
                inode.addrs[j]  = new_data_addr[(size_t)i * JFS_MAX_FILE_BLOCKS + j];
                inode.self      = 0;   /* patched below */
            }
        }
        if (off + 1 > hb) {
            rc = JFS_ERR_NOSPACE;
            break;
        }
        inode.generation++;
        inode.self = (new_half << 32) | off;
        rc = log_write_record(fs, new_half, off, JFS_LREC_INODE, inode.ino, 0,
                              &inode, sizeof(inode), &new_inode_addr[i]);
        if (rc != JFS_OK)
            break;
        off += 1;
        live += JFS_BLOCK_SIZE;
    }

    if (rc == JFS_OK) {
        jfs_crash_check(JFS_CRASH_COMPACT_POST, "records copied, checkpoint pending");

        for (i = 0; i < JFS_MAX_INODES; i++) {
            if (newmap.entries[i].state == JFS_IMAP_USED &&
                new_inode_addr[i] != 0)
                newmap.entries[i].addr = new_inode_addr[i];
        }
        if (off + 1 > hb) {
            rc = JFS_ERR_NOSPACE;
        } else {
            uint64_t imap_addr = 0;
            newmap.crc = 0;
            newmap.crc = jfs_crc_struct(&newmap, sizeof(newmap),
                                        offsetof(jfs_imap, crc));
            rc = log_write_record(fs, new_half, off, JFS_LREC_IMAP, 0, 0,
                                  &newmap, sizeof(newmap), &imap_addr);
            off += 1;
            live += JFS_BLOCK_SIZE;

            if (rc == JFS_OK) {
                jfs_checkpoint nc;
                uint64_t old_seq = fs->ckpt.seq;

                memcpy(&nc, &fs->ckpt, sizeof(nc));
                nc.log_half     = (uint32_t)new_half;
                nc.log_head     = off;
                nc.log_seq      = fs->log_seq;
                nc.imap_addr    = imap_addr;
                nc.seq          = old_seq + 1;
                nc.live_bytes   = live;
                nc.crc          = 0;
                nc.crc = jfs_crc_struct(&nc, sizeof(nc), offsetof(jfs_checkpoint, crc));

                rc = jfs_checkpoint_write_struct(fs, &nc, false);
                if (rc == JFS_OK) {
                    fs->ckpt = nc;
                    fs->imap = newmap;
                    fs->log_live_bytes = live;
                    fs->stats.compactions++;
                    fs->stats.checkpoint_seq = nc.seq;
                }
            }
        }
    }

    free(new_inode_addr);
    free(new_data_addr);
    return rc;
}
