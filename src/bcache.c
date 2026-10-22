/*
 * bcache.c -- Bounded filesystem block buffer cache implementation.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bcache.h"
#include "util.h"

static jfs_bcache g_bcache;

jfs_bcache *jfs_get_bcache(void)
{
    return &g_bcache;
}

static void lru_detach(jfs_bcache *bc, jfs_bcache_entry *e)
{
    if (e->prev)
        e->prev->next = e->next;
    else
        bc->lru_head = e->next;

    if (e->next)
        e->next->prev = e->prev;
    else
        bc->lru_tail = e->prev;

    e->prev = NULL;
    e->next = NULL;
}

static void lru_push_front(jfs_bcache *bc, jfs_bcache_entry *e)
{
    e->prev = NULL;
    e->next = bc->lru_head;
    if (bc->lru_head)
        bc->lru_head->prev = e;
    bc->lru_head = e;
    if (!bc->lru_tail)
        bc->lru_tail = e;
}

static int writeback_entry(jfs_bcache *bc, jfs_bcache_entry *e)
{
    if (!e->valid || !e->dirty)
        return JFS_OK;

    off_t off = (off_t)e->block_no * JFS_BLOCK_SIZE;
    ssize_t n = jfs_pwrite_full(bc->fd, e->data, JFS_BLOCK_SIZE, off);
    if (n != JFS_BLOCK_SIZE)
        return JFS_ERR_IO;

    e->dirty = false;
    bc->stats.writebacks++;
    return JFS_OK;
}

int jfs_bcache_init(jfs_bcache *bc, int fd, uint32_t capacity)
{
    if (!bc)
        return JFS_ERR_INVAL;

    memset(bc, 0, sizeof(*bc));
    bc->fd = fd;
    bc->capacity = (capacity == 0) ? JFS_BCACHE_DEFAULT_FRAMES : capacity;
    bc->entries = (jfs_bcache_entry *)calloc(bc->capacity, sizeof(jfs_bcache_entry));
    if (!bc->entries)
        return JFS_ERR_NOMEM;

    bc->enabled = true;
    return JFS_OK;
}

int jfs_bcache_destroy(jfs_bcache *bc)
{
    if (!bc || !bc->entries)
        return JFS_OK;

    int rc = jfs_bcache_flush_all(bc);
    free(bc->entries);
    bc->entries = NULL;
    bc->capacity = 0;
    bc->count = 0;
    bc->lru_head = NULL;
    bc->lru_tail = NULL;
    bc->enabled = false;
    return rc;
}

static jfs_bcache_entry *find_entry(jfs_bcache *bc, uint64_t block_no)
{
    for (jfs_bcache_entry *curr = bc->lru_head; curr != NULL; curr = curr->next) {
        if (curr->valid && curr->block_no == block_no)
            return curr;
    }
    return NULL;
}

int jfs_bcache_read_block(jfs_bcache *bc, uint64_t block_no, void *buf)
{
    if (!bc || !buf)
        return JFS_ERR_INVAL;

    if (!bc->enabled || !bc->entries) {
        ssize_t n = jfs_pread_full(bc->fd, buf, JFS_BLOCK_SIZE, (off_t)block_no * JFS_BLOCK_SIZE);
        return (n == JFS_BLOCK_SIZE) ? JFS_OK : JFS_ERR_IO;
    }

    jfs_bcache_entry *e = find_entry(bc, block_no);
    if (e) {
        bc->stats.hits++;
        lru_detach(bc, e);
        lru_push_front(bc, e);
        memcpy(buf, e->data, JFS_BLOCK_SIZE);
        return JFS_OK;
    }

    bc->stats.misses++;
    /* Need to load into cache */
    if (bc->count < bc->capacity) {
        e = &bc->entries[bc->count++];
    } else {
        /* Evict LRU tail */
        e = bc->lru_tail;
        if (!e)
            return JFS_ERR_CORRUPT;
        if (e->dirty) {
            int rc = writeback_entry(bc, e);
            if (rc != JFS_OK)
                return rc;
        }
        bc->stats.evictions++;
        lru_detach(bc, e);
    }

    off_t off = (off_t)block_no * JFS_BLOCK_SIZE;
    ssize_t n = jfs_pread_full(bc->fd, e->data, JFS_BLOCK_SIZE, off);
    if (n < 0)
        return JFS_ERR_IO;
    if (n < JFS_BLOCK_SIZE)
        memset(e->data + n, 0, JFS_BLOCK_SIZE - (size_t)n);

    e->block_no = block_no;
    e->valid = true;
    e->dirty = false;
    lru_push_front(bc, e);

    memcpy(buf, e->data, JFS_BLOCK_SIZE);
    return JFS_OK;
}

int jfs_bcache_write_block(jfs_bcache *bc, uint64_t block_no, const void *buf)
{
    if (!bc || !buf)
        return JFS_ERR_INVAL;

    if (!bc->enabled || !bc->entries) {
        ssize_t n = jfs_pwrite_full(bc->fd, buf, JFS_BLOCK_SIZE, (off_t)block_no * JFS_BLOCK_SIZE);
        return (n == JFS_BLOCK_SIZE) ? JFS_OK : JFS_ERR_IO;
    }

    jfs_bcache_entry *e = find_entry(bc, block_no);
    if (e) {
        bc->stats.hits++;
        lru_detach(bc, e);
        lru_push_front(bc, e);
        memcpy(e->data, buf, JFS_BLOCK_SIZE);
        e->dirty = true;
        return JFS_OK;
    }

    bc->stats.misses++;
    if (bc->count < bc->capacity) {
        e = &bc->entries[bc->count++];
    } else {
        e = bc->lru_tail;
        if (!e)
            return JFS_ERR_CORRUPT;
        if (e->dirty) {
            int rc = writeback_entry(bc, e);
            if (rc != JFS_OK)
                return rc;
        }
        bc->stats.evictions++;
        lru_detach(bc, e);
    }

    e->block_no = block_no;
    e->valid = true;
    e->dirty = true;
    memcpy(e->data, buf, JFS_BLOCK_SIZE);
    lru_push_front(bc, e);
    return JFS_OK;
}

int jfs_bcache_flush_range(jfs_bcache *bc, uint64_t start_block, uint64_t end_block)
{
    if (!bc || !bc->entries)
        return JFS_OK;

    int first_err = JFS_OK;
    for (uint32_t i = 0; i < bc->count; i++) {
        jfs_bcache_entry *e = &bc->entries[i];
        if (e->valid && e->dirty && e->block_no >= start_block && e->block_no <= end_block) {
            int rc = writeback_entry(bc, e);
            if (rc != JFS_OK && first_err == JFS_OK)
                first_err = rc;
        }
    }
    bc->stats.flushes++;
    return first_err;
}

int jfs_bcache_flush_all(jfs_bcache *bc)
{
    if (!bc || !bc->entries)
        return JFS_OK;

    int first_err = JFS_OK;
    for (uint32_t i = 0; i < bc->count; i++) {
        jfs_bcache_entry *e = &bc->entries[i];
        if (e->valid && e->dirty) {
            int rc = writeback_entry(bc, e);
            if (rc != JFS_OK && first_err == JFS_OK)
                first_err = rc;
        }
    }
    bc->stats.flushes++;
    return first_err;
}

int jfs_bcache_invalidate_all(jfs_bcache *bc)
{
    if (!bc || !bc->entries)
        return JFS_OK;

    int rc = jfs_bcache_flush_all(bc);
    for (uint32_t i = 0; i < bc->count; i++) {
        bc->entries[i].valid = false;
        bc->entries[i].dirty = false;
        bc->entries[i].prev = NULL;
        bc->entries[i].next = NULL;
    }
    bc->count = 0;
    bc->lru_head = NULL;
    bc->lru_tail = NULL;
    return rc;
}

void jfs_bcache_get_stats(const jfs_bcache *bc, jfs_bcache_stats *out)
{
    if (bc && out)
        *out = bc->stats;
}

void jfs_bcache_reset_stats(jfs_bcache *bc)
{
    if (bc)
        memset(&bc->stats, 0, sizeof(bc->stats));
}
