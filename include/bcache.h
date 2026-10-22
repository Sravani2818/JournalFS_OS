/*
 * bcache.h -- Bounded filesystem block buffer cache for JournalFS (Layer 5/6).
 *
 * Implements a fixed-frame LRU cache mapped to disk-image blocks (4096 bytes).
 * Supports:
 *   - Fixed-size cache frames (default 64 frames = 256 KiB).
 *   - Exact block mapping (block index = byte_offset / JFS_BLOCK_SIZE).
 *   - LRU eviction policy with doubly-linked list.
 *   - Dirty block tracking with write-back on eviction or explicit flush.
 *   - Direct interaction with journal commit ordering and fsync barriers.
 *   - Hit, miss, eviction, and write-back counters.
 */
#ifndef JFS_BCACHE_H
#define JFS_BCACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#include "disk_format.h"

#define JFS_BCACHE_DEFAULT_FRAMES 64u

typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writebacks;
    uint64_t flushes;
} jfs_bcache_stats;

typedef struct jfs_bcache_entry {
    uint64_t block_no;
    bool     valid;
    bool     dirty;
    uint8_t  data[JFS_BLOCK_SIZE];
    struct jfs_bcache_entry *prev;
    struct jfs_bcache_entry *next;
} jfs_bcache_entry;

typedef struct jfs_bcache {
    int               fd;
    uint32_t          capacity;
    uint32_t          count;
    jfs_bcache_entry *entries;
    jfs_bcache_entry *lru_head;   /* Most recently used */
    jfs_bcache_entry *lru_tail;   /* Least recently used (next victim) */
    jfs_bcache_stats  stats;
    bool              enabled;
} jfs_bcache;

/* Initialize the buffer cache with a fixed number of frames. */
int jfs_bcache_init(jfs_bcache *bc, int fd, uint32_t capacity);

/* Destroy and free resources; flushes dirty blocks before freeing. */
int jfs_bcache_destroy(jfs_bcache *bc);

/* Read a single full block from cache (or disk on miss). */
int jfs_bcache_read_block(jfs_bcache *bc, uint64_t block_no, void *buf);

/* Write a single full block to cache (marks frame dirty, updates LRU). */
int jfs_bcache_write_block(jfs_bcache *bc, uint64_t block_no, const void *buf);

/* Flush dirty blocks belonging to a specific block range (inclusive) to disk. */
int jfs_bcache_flush_range(jfs_bcache *bc, uint64_t start_block, uint64_t end_block);

/* Flush all dirty blocks in the cache to disk. */
int jfs_bcache_flush_all(jfs_bcache *bc);

/* Invalidate all cached frames (e.g. on unmount or cache reset). */
int jfs_bcache_invalidate_all(jfs_bcache *bc);

/* Query cache statistics. */
void jfs_bcache_get_stats(const jfs_bcache *bc, jfs_bcache_stats *out);

/* Reset cache statistics. */
void jfs_bcache_reset_stats(jfs_bcache *bc);

/* Global cache accessor for the mounted filesystem. */
jfs_bcache *jfs_get_bcache(void);

#endif /* JFS_BCACHE_H */
