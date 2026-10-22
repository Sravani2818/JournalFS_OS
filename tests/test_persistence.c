/*
 * test_persistence.c -- data persistence tests across restarts in C11.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "test_framework.h"

static int copy_file(const char *src, const char *dst)
{
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) { close(sfd); return -1; }
    char buf[8192];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        if (write(dfd, buf, n) != n) { close(sfd); close(dfd); return -1; }
    }
    close(sfd);
    close(dfd);
    return 0;
}

int main(void)
{
    jfs_test_ctx ctx;
    jfs_test_init(&ctx, "test_persistence");

    char img[512];
    snprintf(img, sizeof(img), "%s/persist.img", ctx.workdir);

    printf("--- basic persistence across restarts ---\n");
    {
        const char *args[] = {"-c", "format --size 4m"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /hello", "-c", "write /hello HELLO", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "first session writes the file", "wrote /hello", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_eq(&ctx, "content survives a restart", "HELLO\n", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "append /hello +MORE", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_eq(&ctx, "appended content survives a restart", "HELLO+MORE\n", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- metadata persistence ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "chmod 640 /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "stat /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "permission bits survive a restart", "(0640)", res.output);
        assert_has(&ctx, "inode number survives a restart", "Inode: 2", res.output);
        assert_has(&ctx, "size survives a restart", "Size: 10", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- many files, nested directories and restarts ---\n");
    char setup_file[512];
    snprintf(setup_file, sizeof(setup_file), "%s/setup.jfs", ctx.workdir);
    FILE *fp = fopen(setup_file, "w");
    if (fp) {
        fprintf(fp, "mount\n");
        fprintf(fp, "mkdir /tree\n");
        for (int i = 1; i <= 5; i++) {
            fprintf(fp, "mkdir /tree/d%d\n", i);
            for (int j = 1; j <= 8; j++) {
                fprintf(fp, "create /tree/d%d/f%d\n", i, j);
                fprintf(fp, "write /tree/d%d/f%d content-%d-%d\n", i, j, i, j);
            }
        }
        fprintf(fp, "unmount\n");
        fclose(fp);
    }
    {
        const char *args[] = {"--quiet", "-f", setup_file};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_not_has(&ctx, "bulk setup reports no errors", "error:", res.output);
        jfs_cmd_res_free(&res);
    }

    char check_file[512];
    snprintf(check_file, sizeof(check_file), "%s/check.jfs", ctx.workdir);
    fp = fopen(check_file, "w");
    if (fp) {
        fprintf(fp, "mount\n");
        for (int i = 1; i <= 5; i++) {
            for (int j = 1; j <= 8; j++) {
                fprintf(fp, "hash /tree/d%d/f%d\n", i, j);
            }
        }
        fprintf(fp, "check\n");
        fprintf(fp, "unmount\n");
        fclose(fp);
    }

    char first_hash_dump[8192] = {0};
    for (int round = 1; round <= 3; round++) {
        const char *args[] = {"--quiet", "-f", check_file};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "consistent after round", "is consistent", res.output);

        if (round == 1) {
            snprintf(first_hash_dump, sizeof(first_hash_dump), "%s", res.output ? res.output : "");
            pass(&ctx, "40 files hashed in round 1");
        } else {
            assert_eq(&ctx, "content hashes identical in round", first_hash_dump, res.output);
        }
        jfs_cmd_res_free(&res);
    }

    printf("--- a read-only session changes no content ---\n");
    char h_before[128] = {0}, h_after[128] = {0};
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "hash /tree/d3/f5"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        char *p = strstr(res.output, "crc32c ");
        if (p) sscanf(p, "crc32c %127s", h_before);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /tree/d3/f5", "-c", "hash /tree/d3/f5"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "content unchanged by reading", "crc32c", res.output);
        char *p = strstr(res.output, "crc32c ");
        if (p) sscanf(p, "crc32c %127s", h_after);
        jfs_cmd_res_free(&res);
        assert_eq(&ctx, "hash before/after a read session", h_before, h_after);
    }

    printf("--- batched checkpoints still persist ---\n");
    char batch_img[512];
    snprintf(batch_img, sizeof(batch_img), "%s/batch.img", ctx.workdir);
    copy_file(img, batch_img);
    {
        const char *args[] = {
            "--quiet", "--checkpoint-interval", "4", "-c", "mount",
            "-c", "create /b1", "-c", "write /b1 one",
            "-c", "create /b2", "-c", "write /b2 two",
            "-c", "create /b3", "-c", "write /b3 three",
            "-c", "unmount"
        };
        jfs_cmd_res res = JFS_RUN_ARR(batch_img, args);
        assert_not_has(&ctx, "batched session has no errors", "error:", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /b1", "-c", "cat /b2", "-c", "cat /b3"};
        jfs_cmd_res res = JFS_RUN_ARR(batch_img, args);
        assert_has(&ctx, "first batched file persisted", "one", res.output);
        assert_has(&ctx, "second batched file persisted", "two", res.output);
        assert_has(&ctx, "third batched file persisted", "three", res.output);
        jfs_cmd_res_free(&res);
        assert_consistent(&ctx, "batched image is consistent", batch_img);
    }

    printf("--- large file survives copies ---\n");
    char large_img[512];
    snprintf(large_img, sizeof(large_img), "%s/large.img", ctx.workdir);
    copy_file(img, large_img);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /big", "-c", "fill /big 60000 Q"};
        jfs_cmd_res res = JFS_RUN_ARR(large_img, args);
        jfs_cmd_res_free(&res);
    }
    char lh1[128] = {0}, lh2[128] = {0};
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "hash /big"};
        jfs_cmd_res res = JFS_RUN_ARR(large_img, args);
        char *p = strstr(res.output, "crc32c ");
        if (p) sscanf(p, "crc32c %127s", lh1);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "hash /big"};
        jfs_cmd_res res = JFS_RUN_ARR(large_img, args);
        char *p = strstr(res.output, "crc32c ");
        if (p) sscanf(p, "crc32c %127s", lh2);
        jfs_cmd_res_free(&res);
        assert_eq(&ctx, "60000 byte file hash is stable across mounts", lh1, lh2);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "stat /big"};
        jfs_cmd_res res = JFS_RUN_ARR(large_img, args);
        assert_has(&ctx, "large file reports 15 blocks", "Blocks: 15", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- deleting survives restarts ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rm /tree/d1/f1"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "ls /tree/d1"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_not_has(&ctx, "deleted file stays deleted", "f1 ", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rm -r /tree"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "recursive delete reports success", "removed /tree", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "ls"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_not_has(&ctx, "the removed tree is gone", "tree", res.output);
        assert_has(&ctx, "only the unrelated file is left", "1 entry", res.output);
        jfs_cmd_res_free(&res);
    }

    return jfs_test_summary(&ctx);
}
