/*
 * disk_format.h -- JournalFS on-disk format definitions.
 *
 * Everything in this header describes bytes that live inside the JournalFS
 * disk image.  All structures are packed and use explicit little-endian
 * layout on the machines this project targets (x86-64 Linux).  The format is
 * documented in docs/disk_format.md; keep the two in sync.
 *
 * Image layout (block = 4096 bytes):
 *
 *   block 0                     superblock
 *   block 1 .. 2                checkpoint slots A / B (double buffered)
 *   block 3 .. 50               journal region (circular, 48 blocks)
 *   block 51 .. end             log region, split into 2 equal halves
 *
 * The log region is the log-structured home of every piece of filesystem
 * data and metadata: inodes, directory blocks, file data blocks and the
 * inode map (imap).  New versions of an object are appended; old versions
 * stay where they are until a checkpoint/compaction reclaims them.
 */
#ifndef JFS_DISK_FORMAT_H
#define JFS_DISK_FORMAT_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Geometry constants                                                  */
/* ------------------------------------------------------------------ */

#define JFS_BLOCK_SIZE          4096u   /* physical block size (bytes)     */
#define JFS_DATA_BYTES          4048u   /* usable bytes per file data blk  */
#define JFS_MAX_FILE_BLOCKS     16u     /* direct blocks per inode         */
#define JFS_MAX_FILE_BYTES      (JFS_MAX_FILE_BLOCKS * JFS_DATA_BYTES)
#define JFS_DIRENTS_PER_BLOCK   100u    /* directory entries per dir block */
#define JFS_MAX_INODES          252u    /* entries in the inode map        */
#define JFS_ROOT_INO            1u      /* well known root directory inode */
#define JFS_NAME_MAX            31u     /* longest file name (bytes)       */
#define JFS_JOURNAL_BLOCKS      48u     /* journal region size             */
#define JFS_CHECKPOINT_BLOCKS   2u      /* double buffered checkpoint      */
#define JFS_SUPERBLOCK_BLOCK    0u
#define JFS_CHECKPOINT_BLOCK    1u
#define JFS_JOURNAL_BLOCK       (JFS_SUPERBLOCK_BLOCK + 1u + JFS_CHECKPOINT_BLOCKS)
#define JFS_LOG_BLOCK           (JFS_JOURNAL_BLOCK + JFS_JOURNAL_BLOCKS)

#define JFS_DEFAULT_SIZE_KIB    4096u   /* default image size = 4 MiB      */
#define JFS_MIN_SIZE_KIB        1024u   /* 1 MiB: log halves must stay sane */
#define JFS_MAX_SIZE_KIB        65536u
#define JFS_MIN_LOG_HALF_BLOCKS 48u     /* refuse images with a tiny log   */

#define JFS_FORMAT_VERSION      1u

/* Magic numbers.  Each region is self describing so that a truncated,
 * overwritten or foreign image is detected instead of trusted. */
#define JFS_SUPER_MAGIC         0x4A534652u  /* "JSFR" */
#define JFS_CKPT_MAGIC          0x4A434B50u  /* "JCKP" */
#define JFS_JREC_MAGIC          0x4A4A524Eu  /* "JJRN" */
#define JFS_LREC_MAGIC          0x4A4C4F47u  /* "JLOG" */
#define JFS_IMAP_MAGIC          0x4A494D50u  /* "JIMP" */
#define JFS_DIRBLK_MAGIC        0x4A444952u  /* "JDIR" */

/* ------------------------------------------------------------------ */
/* Object types, states and flags                                      */
/* ------------------------------------------------------------------ */

#define JFS_TYPE_NONE           0u
#define JFS_TYPE_FILE           1u
#define JFS_TYPE_DIR            2u

#define JFS_IMAP_FREE           0u
#define JFS_IMAP_USED           1u

/* Log record types (records appended to the log region). */
#define JFS_LREC_IMAP           1u
#define JFS_LREC_INODE          2u
#define JFS_LREC_DATA           3u
#define JFS_LREC_DIRBLK         4u

/* Journal record types. */
#define JFS_JREC_OP             1u   /* redo operation payload            */
#define JFS_JREC_COMMIT         2u   /* transaction committed (durability) */
#define JFS_JREC_ABORT          3u   /* transaction aborted               */
#define JFS_JREC_CHECKPOINT     4u   /* informational: checkpoint marker  */

/* Redo operation types inside a JFS_JREC_OP payload. */
#define JFS_OP_INODE            1u   /* create/update/delete an inode      */
#define JFS_OP_DATA             2u   /* write file data block              */
#define JFS_OP_DIRBLK           3u   /* replace a directory data block     */

/* Inode flags. */
#define JFS_INODE_FREE          0x0001u  /* payload marks the inode deleted */
#define JFS_INODE_TYPE_MASK     0x000Eu

/* Dirent flags. */
#define JFS_DE_VALID            0x0001u

/* Superblock flags. */
#define JFS_SB_CLEAN            0x0001u  /* unmounted cleanly last time */

/* Simulated single user model: one user, one group. */
#define JFS_CURRENT_UID         1000u
#define JFS_CURRENT_GID         1000u

/* Permission bits (single user model, POSIX style, 9 bits + type bits) */
#define JFS_S_IRUSR 0400u
#define JFS_S_IWUSR 0200u
#define JFS_S_IXUSR 0100u
#define JFS_S_IRGRP 0040u
#define JFS_S_IWGRP 0020u
#define JFS_S_IXGRP 0010u
#define JFS_S_IROTH 0004u
#define JFS_S_IWOTH 0002u
#define JFS_S_IXOTH 0001u
#define JFS_PERM_BITS 0777u
#define JFS_DEFAULT_FILE_MODE 0644u
#define JFS_DEFAULT_DIR_MODE  0755u

/* ------------------------------------------------------------------ */
/* On-disk structures                                                  */
/* ------------------------------------------------------------------ */

/* Timestamps: seconds + nanoseconds since the Unix epoch. */
typedef struct __attribute__((packed)) {
    int64_t  sec;
    uint32_t nsec;
    uint32_t pad;
} jfs_time;                      /* 16 bytes */

/* Block 0: static geometry, written once by `format`. */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* JFS_SUPER_MAGIC                         */
    uint16_t version;            /* JFS_FORMAT_VERSION                      */
    uint16_t block_size;         /* JFS_BLOCK_SIZE                          */
    uint64_t total_blocks;       /* image size in blocks                    */
    uint64_t checkpoint_block;   /* first checkpoint slot block             */
    uint64_t journal_block;      /* first journal block                     */
    uint32_t journal_blocks;     /* journal region length                   */
    uint64_t log_block;          /* first log block                         */
    uint64_t log_blocks;         /* log region length (both halves)         */
    uint32_t max_inodes;
    uint32_t max_file_bytes;
    uint32_t flags;              /* JFS_SB_CLEAN                            */
    uint32_t image_id;           /* random id, repeated in journal records  */
    jfs_time created_at;
    uint32_t crc;                /* crc32c over this struct with crc = 0    */
    uint8_t  pad[4008];          /* block filler, must be zero              */
} jfs_superblock;                /* 4096 bytes */

/* One imap entry: inode number -> log address of its newest inode record. */
typedef struct __attribute__((packed)) {
    uint32_t ino;
    uint32_t state;              /* JFS_IMAP_FREE / JFS_IMAP_USED           */
    uint64_t addr;               /* log address of the inode record         */
} jfs_imap_entry;                /* 16 bytes */

/* The inode map: a single log record that maps inode numbers to the current
 * version of each inode.  Rewritten (copy-on-write, appended) whenever any
 * inode address changes. */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* JFS_IMAP_MAGIC                          */
    uint32_t count;              /* number of USED entries                  */
    uint32_t next_ino;           /* next candidate inode number             */
    uint32_t crc;                /* crc32c over the struct with crc = 0     */
    jfs_imap_entry entries[JFS_MAX_INODES];
} jfs_imap;                      /* 16 + 252*16 = 4048 bytes */

/* Inode: metadata for one file or directory.  Data blocks are referenced by
 * log address; blklen records how many bytes of each block are meaningful
 * (the last block of a file may be partially filled). */
typedef struct __attribute__((packed)) {
    uint32_t ino;                /* inode number (0 = unused)               */
    uint16_t type;               /* JFS_TYPE_FILE / JFS_TYPE_DIR            */
    uint16_t mode;               /* permission bits, JFS_PERM_BITS          */
    uint32_t uid;                /* owner id (single user model: 0)         */
    uint32_t gid;                /* group id                                */
    uint32_t flags;              /* JFS_INODE_FREE etc.                     */
    uint32_t generation;         /* bumped on every rewrite of this inode   */
    uint64_t size;               /* logical size in bytes                   */
    jfs_time ctime;              /* creation time                           */
    jfs_time mtime;              /* last modification time                  */
    uint32_t nblocks;            /* number of valid entries in addrs[]      */
    uint32_t linkcount;          /* 1 for files, 2 + nsubdir for dirs       */
    uint64_t self;               /* log address of this inode record        */
    uint64_t addrs[JFS_MAX_FILE_BLOCKS];   /* log addresses of data blocks   */
    uint32_t blklen[JFS_MAX_FILE_BLOCKS];  /* meaningful bytes in each block */
    uint32_t provided_mask;      /* transaction only: addrs[] set by this tx.
                                  * Always written as 0 into the log.       */
} jfs_inode;                     /* 276 bytes */

/* Directory entry.  Directories store names -> inode numbers; the data of a
 * file is never stored inside a directory entry. */
typedef struct __attribute__((packed)) {
    char     name[JFS_NAME_MAX + 1];  /* NUL terminated when shorter         */
    uint32_t ino;
    uint16_t type;                    /* JFS_TYPE_FILE / JFS_TYPE_DIR        */
    uint16_t flags;                   /* JFS_DE_VALID                        */
} jfs_dirent;                    /* 40 bytes */

/* One block of directory data (the data blocks of a directory inode). */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* JFS_DIRBLK_MAGIC                        */
    uint32_t count;              /* number of VALID entries                 */
    uint32_t crc;                /* crc32c over the struct with crc = 0     */
    uint32_t pad;
    jfs_dirent ents[JFS_DIRENTS_PER_BLOCK];
} jfs_dirblk;                    /* 16 + 100*40 = 4016 bytes */

/* ------------------------------------------------------------------ */
/* Log record (log region, append only)                                */
/* ------------------------------------------------------------------ */

/*
 * Every log record occupies whole blocks.  The 48 byte header sits at the
 * start of the first block and the payload follows immediately, so a full
 * data block record (4048 payload bytes) fits exactly in one 4096 byte
 * block: 48 + 4048 = 4096.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* JFS_LREC_MAGIC                          */
    uint16_t version;            /* JFS_FORMAT_VERSION                      */
    uint16_t type;               /* JFS_LREC_*                              */
    uint64_t addr;               /* self address (block offset in half)     */
    uint64_t seq;                /* append sequence (monotonic per image)   */
    uint32_t obj_ino;            /* object this record belongs to           */
    uint32_t block_index;        /* block index inside the object           */
    uint32_t payload_len;        /* bytes of payload                        */
    uint32_t payload_crc;        /* crc32c over the payload                 */
    uint32_t image_id;           /* superblock image id                     */
    uint32_t hdr_crc;            /* crc32c over this header with hdr_crc=0  */
} jfs_log_rec_hdr;               /* 48 bytes */

/* ------------------------------------------------------------------ */
/* Journal record (journal region, circular)                           */
/* ------------------------------------------------------------------ */

/*
 * A transaction writes one or more JFS_JREC_OP records (each holding a list
 * of redo operations) followed by exactly one JFS_JREC_COMMIT record.  The
 * commit record is the only thing that makes the transaction durable.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* JFS_JREC_MAGIC                          */
    uint16_t version;            /* JFS_FORMAT_VERSION                      */
    uint16_t type;               /* JFS_JREC_*                              */
    uint64_t txid;               /* transaction id                          */
    uint64_t lsn;                /* global record sequence number           */
    uint32_t seq;                /* record index inside the transaction     */
    uint32_t nblocks;            /* blocks occupied by this record          */
    uint32_t payload_len;        /* payload bytes                           */
    uint32_t payload_crc;        /* crc32c over the payload                 */
    uint32_t image_id;           /* must match the superblock               */
    uint32_t ops;                /* number of redo ops in the payload       */
    uint32_t hdr_crc;            /* crc32c over this header with hdr_crc=0  */
    uint32_t flags;
} jfs_journal_rec_hdr;           /* 56 bytes */

/* Redo operation header; `length` payload bytes follow it. */
typedef struct __attribute__((packed)) {
    uint16_t op;                 /* JFS_OP_*                                */
    uint16_t flags;
    uint32_t ino;                /* target object                           */
    uint32_t index;              /* block index for DATA/DIRBLK             */
    uint32_t length;             /* payload bytes that follow               */
    uint32_t crc;                /* crc32c over the payload                 */
} jfs_redo_op_hdr;               /* 20 bytes */

/* Block 1 or 2: double buffered checkpoint.  The slot with the highest
 * valid `seq` wins; a torn slot simply fails its checksum and is ignored. */
typedef struct __attribute__((packed)) {
    uint32_t magic;              /* JFS_CKPT_MAGIC                          */
    uint16_t version;            /* JFS_FORMAT_VERSION                      */
    uint16_t flags;
    uint64_t seq;                /* monotonically increasing                */
    uint64_t applied_txid;       /* every txid <= this is applied to log    */
    uint32_t log_half;           /* 0 or 1: which half is active            */
    uint64_t log_head;           /* blocks used in the active half          */
    uint64_t log_seq;            /* last log record sequence appended       */
    uint64_t imap_addr;          /* log address of the current imap record  */
    uint64_t next_txid;
    uint64_t journal_start;      /* first journal block of the live window  */
    uint32_t journal_used;       /* journal blocks in use (diagnostics)     */
    uint32_t n_inodes;           /* inodes in use                           */
    uint64_t live_bytes;         /* bytes of live records (diagnostics)     */
    uint64_t replayed_total;     /* transactions replayed over the lifetime */
    uint32_t crc;                /* crc32c over this struct with crc = 0    */
    uint8_t  pad[4000];
} jfs_checkpoint;                /* 4096 bytes */

/* Compile time sanity checks: the on-disk layout must match the docs. */
#define JFS_SA(cond, tag) typedef char jfs_static_assert_##tag[(cond) ? 1 : -1]
JFS_SA(sizeof(jfs_superblock) == 4096, superblock_size);
JFS_SA(sizeof(jfs_checkpoint) == 4096, checkpoint_size);
JFS_SA(sizeof(jfs_imap) == 4048, imap_size);
JFS_SA(sizeof(jfs_inode) == 276, inode_size);
JFS_SA(sizeof(jfs_dirent) == 40, dirent_size);
JFS_SA(sizeof(jfs_dirblk) == 4016, dirblk_size);
JFS_SA(sizeof(jfs_log_rec_hdr) == 48, log_rec_hdr_size);
JFS_SA(sizeof(jfs_journal_rec_hdr) == 56, journal_rec_hdr_size);
JFS_SA(sizeof(jfs_redo_op_hdr) == 20, redo_op_hdr_size);
JFS_SA(sizeof(jfs_log_rec_hdr) + JFS_DATA_BYTES == JFS_BLOCK_SIZE, data_rec_fits);
JFS_SA(sizeof(jfs_log_rec_hdr) + sizeof(jfs_imap) == JFS_BLOCK_SIZE, imap_rec_fits);
JFS_SA(sizeof(jfs_log_rec_hdr) + sizeof(jfs_dirblk) <= JFS_BLOCK_SIZE, dir_rec_fits);
JFS_SA(sizeof(jfs_log_rec_hdr) + sizeof(jfs_inode) <= JFS_BLOCK_SIZE, inode_rec_fits);

#endif /* JFS_DISK_FORMAT_H */
