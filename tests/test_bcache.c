/*
 * test_bcache.c -- Unit and integration tests for bounded block buffer cache (Layer 5/6).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "test_framework.h"
#include "util.h"
#include "bcache.h"
#include "disk_format.h"

int main(void)
{
    jfs_test_ctx ctx;
    jfs_test_init(&ctx, "test_bcache");

    char img[512];
    snprintf(img, sizeof(img), "%s/bcache_unit.img", ctx.workdir);

    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail(&ctx, "create test backing file", "fd >= 0", "open failed");
        return jfs_test_summary(&ctx);
    }
    if (ftruncate(fd, 1024 * 1024) != 0) { /* 1 MiB = 256 blocks */
        close(fd);
        fail(&ctx, "ftruncate test backing file", "rc == 0", "truncate failed");
        return jfs_test_summary(&ctx);
    }

    printf("--- unit test: buffer cache initialization ---\n");
    jfs_bcache bc;
    int rc = jfs_bcache_init(&bc, fd, 4); /* capacity: 4 blocks */
    assert_status(&ctx, "bcache_init succeeds with 4 frames", JFS_OK, rc);

    printf("--- unit test: write and read hits / misses ---\n");
    uint8_t wbuf1[JFS_BLOCK_SIZE], wbuf2[JFS_BLOCK_SIZE], rbuf[JFS_BLOCK_SIZE];
    memset(wbuf1, 'A', sizeof(wbuf1));
    memset(wbuf2, 'B', sizeof(wbuf2));

    rc = jfs_bcache_write_block(&bc, 10, wbuf1);
    assert_status(&ctx, "write block 10", JFS_OK, rc);

    rc = jfs_bcache_read_block(&bc, 10, rbuf);
    assert_status(&ctx, "read block 10", JFS_OK, rc);
    if (memcmp(rbuf, wbuf1, sizeof(rbuf)) == 0)
        pass(&ctx, "read block 10 returned written data");
    else
        fail(&ctx, "read block 10 returned written data", "all A", "mismatched");

    jfs_bcache_stats st;
    jfs_bcache_get_stats(&bc, &st);
    if (st.hits >= 1 && st.misses >= 1)
        pass(&ctx, "hits and misses recorded correctly");
    else
        fail(&ctx, "hits and misses recorded correctly", "hits>=1, misses>=1", "bad stats");

    printf("--- unit test: LRU eviction with dirty writeback ---\n");
    /* Fill remaining frames: currently block 10 is in cache (1 frame used) */
    uint8_t blk_c[JFS_BLOCK_SIZE];
    memset(blk_c, 'C', sizeof(blk_c));
    jfs_bcache_write_block(&bc, 11, blk_c);
    jfs_bcache_write_block(&bc, 12, blk_c);
    jfs_bcache_write_block(&bc, 13, blk_c);
    /* Now cache has: 13 (MRU), 12, 11, 10 (LRU) */
    /* Access block 10 to make it MRU: order becomes 10, 13, 12, 11 (LRU) */
    jfs_bcache_read_block(&bc, 10, rbuf);

    /* Now write block 14 -> should evict 11 */
    uint8_t blk_d[JFS_BLOCK_SIZE];
    memset(blk_d, 'D', sizeof(blk_d));
    jfs_bcache_write_block(&bc, 14, blk_d);

    jfs_bcache_get_stats(&bc, &st);
    if (st.evictions == 1 && st.writebacks >= 1)
        pass(&ctx, "LRU victim evicted with writeback");
    else
        fail(&ctx, "LRU victim evicted with writeback", "evictions=1", "incorrect");

    /* Verify block 11 was flushed to backing file on eviction */
    uint8_t disk_read[JFS_BLOCK_SIZE];
    ssize_t n1 = pread(fd, disk_read, JFS_BLOCK_SIZE, (off_t)11 * JFS_BLOCK_SIZE);
    if (n1 == JFS_BLOCK_SIZE && memcmp(disk_read, blk_c, JFS_BLOCK_SIZE) == 0)
        pass(&ctx, "evicted dirty block 11 persisted to backing disk");
    else
        fail(&ctx, "evicted dirty block 11 persisted to backing disk", "all C", "mismatch");

    printf("--- unit test: explicit flush ---\n");
    rc = jfs_bcache_flush_all(&bc);
    assert_status(&ctx, "jfs_bcache_flush_all succeeds", JFS_OK, rc);

    /* Verify block 14 is on disk */
    ssize_t n2 = pread(fd, disk_read, JFS_BLOCK_SIZE, (off_t)14 * JFS_BLOCK_SIZE);
    if (n2 == JFS_BLOCK_SIZE && memcmp(disk_read, blk_d, JFS_BLOCK_SIZE) == 0)
        pass(&ctx, "flushed dirty block 14 persisted to backing disk");
    else
        fail(&ctx, "flushed dirty block 14 persisted to backing disk", "all D", "mismatch");

    jfs_bcache_destroy(&bc);
    close(fd);

    printf("--- integration test: bcache counters in CLI stats ---\n");
    char fs_img[512];
    snprintf(fs_img, sizeof(fs_img), "%s/bcache_cli.img", ctx.workdir);
    {
        const char *args[] = {"-c", "format --size 2m"};
        jfs_cmd_res res = JFS_RUN_ARR(fs_img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {
            "--quiet", "-c", "mount",
            "-c", "create /cache_test",
            "-c", "write /cache_test HELLO_CACHE",
            "-c", "cat /cache_test",
            "-c", "stats",
            "-c", "unmount"
        };
        jfs_cmd_res res = JFS_RUN_ARR(fs_img, args);
        assert_has(&ctx, "cat verifies cached content", "HELLO_CACHE", res.output);
        assert_has(&ctx, "stats outputs bcache_hits", "bcache_hits,", res.output);
        assert_has(&ctx, "stats outputs bcache_misses", "bcache_misses,", res.output);
        assert_has(&ctx, "stats outputs bcache_evictions", "bcache_evictions,", res.output);
        assert_has(&ctx, "stats outputs bcache_writebacks", "bcache_writebacks,", res.output);
        assert_has(&ctx, "stats outputs bcache_flushes", "bcache_flushes,", res.output);
        jfs_cmd_res_free(&res);
    }

    return jfs_test_summary(&ctx);
}
