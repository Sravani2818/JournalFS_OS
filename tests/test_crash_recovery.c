/*
 * test_crash_recovery.c -- deterministic crash injection and recovery in C11.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "test_framework.h"
#include "corrupt_tool.h"
#include "checksum.h"

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

static void setup_image(const char *img)
{
    unlink(img);
    {
        const char *args[] = {"-c", "format --size 2m"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /f", "-c", "write /f HELLO", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
}

static void recover_and_read(const char *img, char *out_buf, size_t out_sz)
{
    const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f", "-c", "check"};
    jfs_cmd_res res = JFS_RUN_ARR(img, args);
    /* Replace newlines with | */
    size_t pos = 0;
    if (res.output) {
        for (size_t i = 0; res.output[i] != '\0' && pos + 1 < out_sz; i++) {
            if (res.output[i] == '\n')
                out_buf[pos++] = '|';
            else
                out_buf[pos++] = res.output[i];
        }
    }
    out_buf[pos] = '\0';
    jfs_cmd_res_free(&res);
}

int main(void)
{
    jfs_test_ctx ctx;
    jfs_test_init(&ctx, "test_crash_recovery");

    char img[512];
    snprintf(img, sizeof(img), "%s/crash.img", ctx.workdir);

    printf("--- crash before the durable commit: the old value must win ---\n");
    const char *pre_commit_points[] = {"pre_write", "post_write", "post_sync"};
    for (int i = 0; i < 3; i++) {
        const char *p = pre_commit_points[i];
        setup_image(img);

        const char *args[] = {"--crash", p, "-c", "mount", "-c", "write /f JOURNALFS", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        char desc[128];
        snprintf(desc, sizeof(desc), "%s: crash was injected", p);
        assert_status(&ctx, desc, 99, res.status);
        jfs_cmd_res_free(&res);

        char r1[512] = {0}, r2[512] = {0};
        recover_and_read(img, r1, sizeof(r1));
        snprintf(desc, sizeof(desc), "%s: file still holds the old value", p);
        if (strstr(r1, "HELLO|check: filesystem is consistent") != NULL)
            pass(&ctx, desc);
        else
            fail(&ctx, desc, "HELLO|check: ... is consistent", r1);

        recover_and_read(img, r2, sizeof(r2));
        snprintf(desc, sizeof(desc), "%s: recovery is idempotent", p);
        assert_eq(&ctx, desc, r1, r2);
    }

    printf("--- crash after the durable commit: the new value must be recovered ---\n");
    const char *post_commit_points[] = {
        "post_commit_sync", "post_apply", "after_imap", "checkpoint_mid", "checkpoint_post"
    };
    for (int i = 0; i < 5; i++) {
        const char *p = post_commit_points[i];
        setup_image(img);

        const char *args[] = {"--crash", p, "-c", "mount", "-c", "write /f JOURNALFS"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        char desc[128];
        snprintf(desc, sizeof(desc), "%s: crash was injected", p);
        assert_status(&ctx, desc, 99, res.status);
        jfs_cmd_res_free(&res);

        char r1[512] = {0}, r2[512] = {0};
        recover_and_read(img, r1, sizeof(r1));
        snprintf(desc, sizeof(desc), "%s: committed change replayed", p);
        if (strstr(r1, "JOURNALFS|check: filesystem is consistent") != NULL)
            pass(&ctx, desc);
        else
            fail(&ctx, desc, "JOURNALFS|check: ... is consistent", r1);

        recover_and_read(img, r2, sizeof(r2));
        snprintf(desc, sizeof(desc), "%s: recovery is idempotent", p);
        assert_eq(&ctx, desc, r1, r2);
    }

    printf("--- crash between writing and syncing the commit record ---\n");
    {
        setup_image(img);
        const char *args[] = {"--crash", "post_commit", "-c", "mount", "-c", "write /f JOURNALFS"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_status(&ctx, "post_commit: crash was injected", 99, res.status);
        jfs_cmd_res_free(&res);

        char r1[512] = {0};
        recover_and_read(img, r1, sizeof(r1));
        if (strstr(r1, "JOURNALFS|check: filesystem is consistent"))
            pass(&ctx, "post_commit: recorded commit replayed (a process crash keeps the page cache)");
        else if (strstr(r1, "HELLO|check: filesystem is consistent"))
            pass(&ctx, "post_commit: un-synced commit was lost, old value kept (also legal)");
        else
            fail(&ctx, "post_commit: state is old or new, never mixed", "HELLO or JOURNALFS, consistent", r1);
    }

    printf("--- crash while recovering ---\n");
    {
        setup_image(img);
        const char *args1[] = {"--crash", "post_commit_sync", "-c", "mount", "-c", "write /f JOURNALFS"};
        jfs_cmd_res res1 = JFS_RUN_ARR(img, args1);
        jfs_cmd_res_free(&res1);

        const char *args2[] = {"--crash", "recover_mid", "-c", "mount"};
        jfs_cmd_res res2 = JFS_RUN_ARR(img, args2);
        assert_status(&ctx, "recover_mid: crash during recovery was injected", 99, res2.status);
        jfs_cmd_res_free(&res2);

        char r1[512] = {0};
        recover_and_read(img, r1, sizeof(r1));
        if (strstr(r1, "JOURNALFS|check: filesystem is consistent"))
            pass(&ctx, "recover_mid: the next mount finishes the replay");
        else
            fail(&ctx, "recover_mid: the next mount finishes the replay", "JOURNALFS|check: ... consistent", r1);
    }

    printf("--- an uncommitted transaction must not be exposed ---\n");
    {
        setup_image(img);
        const char *args[] = {"--crash", "post_sync", "-c", "mount", "-c", "create /ghost", "-c", "write /ghost boo"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_status(&ctx, "ghost: crash was injected", 99, res.status);
        jfs_cmd_res_free(&res);

        const char *args2[] = {"--quiet", "-c", "mount", "-c", "ls", "-c", "stat /ghost"};
        jfs_cmd_res res2 = JFS_RUN_ARR(img, args2);
        assert_not_has(&ctx, "the uncommitted file does not exist", "Inode:", res2.output);
        assert_has(&ctx, "the uncommitted file is reported missing", "no such file or directory", res2.output);
        jfs_cmd_res_free(&res2);
    }

    printf("--- a session that crashes before its commit, followed by one that commits ---\n");
    {
        setup_image(img);
        const char *args1[] = {"--quiet", "--crash", "post_sync", "-c", "mount", "-c", "append /f WORLD"};
        jfs_cmd_res res1 = JFS_RUN_ARR(img, args1);
        jfs_cmd_res_free(&res1);

        const char *args2[] = {"--quiet", "--crash", "post_commit_sync", "-c", "mount", "-c", "append /f WORLD"};
        jfs_cmd_res res2 = JFS_RUN_ARR(img, args2);
        jfs_cmd_res_free(&res2);

        const char *args3[] = {"-c", "mount", "-c", "cat /f", "-c", "check"};
        jfs_cmd_res res3 = JFS_RUN_ARR(img, args3);
        assert_has(&ctx, "an older incomplete record does not hide a newer commit", "HELLOWORLD", res3.output);
        assert_not_has(&ctx, "the leftover record is not mistaken for damage", "damaged journal record", res3.output);
        assert_has(&ctx, "the image is consistent after both crashes", "is consistent", res3.output);
        jfs_cmd_res_free(&res3);
    }

    printf("--- crash, recover, crash again ---\n");
    {
        setup_image(img);
        const char *args1[] = {"--crash", "post_commit_sync", "-c", "mount", "-c", "write /f first"};
        jfs_cmd_res res1 = JFS_RUN_ARR(img, args1);
        jfs_cmd_res_free(&res1);

        const char *args2[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res2 = JFS_RUN_ARR(img, args2);
        assert_eq(&ctx, "first crash recovered by the next mount", "first\n", res2.output);
        jfs_cmd_res_free(&res2);

        const char *args3[] = {"--crash", "checkpoint_post", "-c", "mount", "-c", "append /f second"};
        jfs_cmd_res res3 = JFS_RUN_ARR(img, args3);
        jfs_cmd_res_free(&res3);

        char r1[512] = {0};
        recover_and_read(img, r1, sizeof(r1));
        if (strncmp(r1, "firstsecond|", 12) == 0)
            pass(&ctx, "two crashes in a row keep both committed changes");
        else
            fail(&ctx, "two crashes in a row keep both committed changes", "firstsecond|check: ... consistent", r1);
    }

    printf("--- batched checkpoints: recovery replays several transactions ---\n");
    {
        setup_image(img);
        const char *args1[] = {
            "--quiet", "--checkpoint-interval", "8", "--crash-after", "3", "--crash", "post_commit_sync",
            "-c", "mount", "-c", "append /f -1", "-c", "append /f -2", "-c", "append /f -3"
        };
        jfs_cmd_res res1 = JFS_RUN_ARR(img, args1);
        assert_status(&ctx, "batched crash session crashed at the third commit", 99, res1.status);
        jfs_cmd_res_free(&res1);

        const char *args2[] = {"-v", "-c", "mount", "-c", "cat /f", "-c", "journal"};
        jfs_cmd_res res2 = JFS_RUN_ARR(img, args2);
        assert_has(&ctx, "all three committed appends were replayed", "HELLO-1-2-3", res2.output);
        assert_has(&ctx, "mount reports the replay statistics", "replayed", res2.output);
        assert_has(&ctx, "recovery replayed more than one transaction", "3 committed and replayed", res2.output);
        jfs_cmd_res_free(&res2);

        const char *args3[] = {"--quiet", "-c", "mount", "-c", "journal"};
        jfs_cmd_res res3 = JFS_RUN_ARR(img, args3);
        assert_has(&ctx, "journal window is empty after the replay", "used=0 of", res3.output);
        jfs_cmd_res_free(&res3);
    }

    printf("--- batched checkpoints with a smaller interval ---\n");
    {
        setup_image(img);
        const char *args1[] = {
            "--quiet", "--checkpoint-interval", "4", "--crash-after", "3", "--crash", "post_commit_sync",
            "-c", "mount", "-c", "append /f a", "-c", "append /f b", "-c", "append /f c"
        };
        jfs_cmd_res res1 = JFS_RUN_ARR(img, args1);
        jfs_cmd_res_free(&res1);

        const char *args2[] = {"-v", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res2 = JFS_RUN_ARR(img, args2);
        assert_has(&ctx, "batched appends are all present", "HELLOabc", res2.output);
        assert_has(&ctx, "mount reports the replayed transactions", "replayed", res2.output);
        jfs_cmd_res_free(&res2);
    }

    printf("--- crash during log compaction ---\n");
    char bigimg[512];
    snprintf(bigimg, sizeof(bigimg), "%s/big.img", ctx.workdir);
    unlink(bigimg);
    {
        const char *args[] = {"-c", "format --size 1m"};
        jfs_cmd_res res = JFS_RUN_ARR(bigimg, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /big", "-c", "fill /big 40000 A", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(bigimg, args);
        jfs_cmd_res_free(&res);
    }

    const char *compact_points[] = {"compact_mid", "compact_post"};
    const char *letters[] = {"B", "C", "D", "E", "F", "G", "H", "I", "J"};
    for (int p_idx = 0; p_idx < 2; p_idx++) {
        const char *P = compact_points[p_idx];
        char kimg[512];
        snprintf(kimg, sizeof(kimg), "%s/k_%s.img", ctx.workdir, P);
        copy_file(bigimg, kimg);

        char crashed_at[16] = {0};
        for (int l = 0; l < 9; l++) {
            char cmd_fill[64];
            snprintf(cmd_fill, sizeof(cmd_fill), "fill /big 40000 %s", letters[l]);
            const char *args[] = {"--quiet", "--crash", P, "-c", "mount", "-c", cmd_fill};
            jfs_cmd_res res = JFS_RUN_ARR(kimg, args);
            if (res.status == 99) {
                snprintf(crashed_at, sizeof(crashed_at), "%s", letters[l]);
                jfs_cmd_res_free(&res);
                break;
            }
            jfs_cmd_res_free(&res);
        }

        char desc[128];
        snprintf(desc, sizeof(desc), "%s: crash during compaction was injected", P);
        if (crashed_at[0]) {
            pass(&ctx, desc);
        } else {
            fail(&ctx, desc, "crash in 9 rounds", "no crash");
            continue;
        }

        char r1[512] = {0};
        {
            const char *args_c[] = {"--quiet", "-c", "mount", "-c", "hash /big", "-c", "check"};
            jfs_cmd_res res_c = JFS_RUN_ARR(kimg, args_c);
            size_t pos = 0;
            if (res_c.output) {
                for (size_t i = 0; res_c.output[i] != '\0' && pos + 1 < sizeof(r1); i++) {
                    if (res_c.output[i] == '\n') r1[pos++] = '|';
                    else r1[pos++] = res_c.output[i];
                }
            }
            r1[pos] = '\0';
            jfs_cmd_res_free(&res_c);
        }
        snprintf(desc, sizeof(desc), "%s: image is consistent after the crash", P);
        if (strstr(r1, "is consistent"))
            pass(&ctx, desc);
        else
            fail(&ctx, desc, "consistent", r1);

        /* Calculate expected CRC32C of crashed_at repeated 40000 times */
        uint8_t blk_buf[40000];
        memset(blk_buf, crashed_at[0], sizeof(blk_buf));
        uint32_t exp_crc = jfs_crc32c(blk_buf, sizeof(blk_buf));
        char exp_str[64];
        snprintf(exp_str, sizeof(exp_str), "0x%08x", exp_crc);

        char got_str[64] = {0};
        char *cp = strstr(r1, "crc32c ");
        if (cp) sscanf(cp, "crc32c %63s", got_str);

        snprintf(desc, sizeof(desc), "%s: recovered content is the last committed version", P);
        assert_eq(&ctx, desc, exp_str, got_str);

        char r2[512] = {0};
        {
            const char *args_c[] = {"--quiet", "-c", "mount", "-c", "hash /big", "-c", "check"};
            jfs_cmd_res res_c = JFS_RUN_ARR(kimg, args_c);
            size_t pos = 0;
            if (res_c.output) {
                for (size_t i = 0; res_c.output[i] != '\0' && pos + 1 < sizeof(r2); i++) {
                    if (res_c.output[i] == '\n') r2[pos++] = '|';
                    else r2[pos++] = res_c.output[i];
                }
            }
            r2[pos] = '\0';
            jfs_cmd_res_free(&res_c);
        }
        char got2_str[64] = {0};
        cp = strstr(r2, "crc32c ");
        if (cp) sscanf(cp, "crc32c %63s", got2_str);

        snprintf(desc, sizeof(desc), "%s: recovery is idempotent after compaction crash", P);
        assert_eq(&ctx, desc, got_str, got2_str);

        snprintf(desc, sizeof(desc), "%s: still consistent after the second mount", P);
        if (strstr(r2, "is consistent"))
            pass(&ctx, desc);
        else
            fail(&ctx, desc, "consistent", r2);
    }

    printf("--- format crash leaves the previous image alone ---\n");
    {
        setup_image(img);
        /* Check image size and checksum */
        int fd = open(img, O_RDONLY);
        uint8_t pre_data[65536];
        ssize_t pre_bytes = read(fd, pre_data, sizeof(pre_data));
        uint32_t pre_crc = (pre_bytes > 0) ? jfs_crc32c(pre_data, pre_bytes) : 0;
        close(fd);

        const char *args[] = {"--crash", "pre_format", "-c", "format --size 4m --force"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_status(&ctx, "pre_format: crash was injected", 99, res.status);
        jfs_cmd_res_free(&res);

        fd = open(img, O_RDONLY);
        uint8_t post_data[65536];
        ssize_t post_bytes = read(fd, post_data, sizeof(post_data));
        uint32_t post_crc = (post_bytes > 0) ? jfs_crc32c(post_data, post_bytes) : 0;
        close(fd);

        if (pre_bytes == post_bytes && pre_crc == post_crc)
            pass(&ctx, "pre_format: the image is byte for byte unchanged");
        else
            fail(&ctx, "pre_format: the image is byte for byte unchanged", "identical", "different");

        char r1[512] = {0};
        recover_and_read(img, r1, sizeof(r1));
        assert_has(&ctx, "pre_format: the filesystem still mounts", "HELLO", r1);
    }

    printf("--- a damaged checkpoint slot falls back to the other slot ---\n");
    {
        setup_image(img);
        const char *args1[] = {"--quiet", "-c", "mount", "-c", "write /f SECOND"};
        jfs_cmd_res res1 = JFS_RUN_ARR(img, args1);
        jfs_cmd_res_free(&res1);

        jfs_corrupt_ckpt_zero(img, 1);

        const char *args2[] = {"-c", "mount", "-c", "check"};
        jfs_cmd_res res2 = JFS_RUN_ARR(img, args2);
        assert_has(&ctx, "mount works with one damaged checkpoint slot", "is consistent", res2.output);
        jfs_cmd_res_free(&res2);
    }

    return jfs_test_summary(&ctx);
}
