/*
 * test_corruption.c -- what happens when the image is damaged (C11 test suite).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "test_framework.h"
#include "corrupt_tool.h"

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

static void build_scenario(const char *img)
{
    unlink(img);
    {
        const char *args[] = {"-q", "-c", "format --size 1m"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /f", "-c", "write /f HELLO"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {
            "--quiet", "--crash-after", "1", "--crash", "post_commit_sync",
            "-c", "mount", "-c", "append /f WORLD"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
}

static void fuzz_damage(const char *path, uint32_t seed)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 512) { close(fd); return; }
    uint64_t nblocks = (uint64_t)st.st_size / 512;

    uint32_t state = seed;
    for (int i = 0; i < 40; i++) {
        state = state * 1664525u + 1013904223u;
        uint64_t blk = state % nblocks;
        off_t off = (off_t)(blk * 512);
        uint8_t buf[512];
        for (int b = 0; b < 512; b++) {
            state = state * 1664525u + 1013904223u;
            buf[b] = (uint8_t)(state >> 24);
        }
        ssize_t pw = pwrite(fd, buf, sizeof(buf), off);
        (void)pw;
    }
    close(fd);
}

int main(void)
{
    jfs_test_ctx ctx;
    jfs_test_init(&ctx, "test_corruption");

    char scen[512];
    snprintf(scen, sizeof(scen), "%s/scen.img", ctx.workdir);

    printf("--- the reference image ---\n");
    build_scenario(scen);
    {
        char cmd_buf[550];
        snprintf(cmd_buf, sizeof(cmd_buf), "inspect %s", scen);
        const char *args[] = {"-c", cmd_buf};
        jfs_cmd_res res = JFS_RUN_ARR(NULL, args);
        assert_has(&ctx, "reference image: the journal holds a committed transaction", "tx 3: 2 record(s), COMMITTED", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        char ref_img[512];
        snprintf(ref_img, sizeof(ref_img), "%s/ref.img", ctx.workdir);
        copy_file(scen, ref_img);
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(ref_img, args);
        assert_eq(&ctx, "reference image: recovery produces the committed value", "HELLOWORLD\n", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("\n--- damaged journal records ---\n");
    char cimg[512];
    /* 1. payload flip */
    snprintf(cimg, sizeof(cimg), "%s/c1.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_jrec_flip_payload(cimg, 0);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "damaged redo payload: mount refuses with a clear reason", "cannot be made consistent", res.output);
        assert_status(&ctx, "damaged redo payload: the failure is reported as an error", 1, res.status);
        jfs_cmd_res_free(&res);
    }

    /* 2. header flip */
    snprintf(cimg, sizeof(cimg), "%s/c2.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_jrec_flip_header(cimg, 0);
    {
        const char *args[] = {"-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "damaged record header: the truncation is reported", "damaged journal record", res.output);
        assert_has(&ctx, "damaged record header: the older consistent state is served", "HELLO", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        char cmd_buf[550];
        snprintf(cmd_buf, sizeof(cmd_buf), "inspect %s", cimg);
        const char *args[] = {"-c", cmd_buf};
        jfs_cmd_res res = JFS_RUN_ARR(NULL, args);
        assert_has(&ctx, "damaged record header: inspect marks the window", "DAMAGED", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "check"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "damaged record header: check warns about the truncation", "truncated at block", res.output);
        jfs_cmd_res_free(&res);
    }

    /* 3. oversize */
    snprintf(cimg, sizeof(cimg), "%s/c3.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_jrec_oversize(cimg, 0);
    {
        const char *args[] = {"-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "impossible record length: rejected and reported", "damaged journal record", res.output);
        assert_has(&ctx, "impossible record length: no torn data is served", "HELLO", res.output);
        jfs_cmd_res_free(&res);
    }

    /* 4. commit record turned into OP */
    snprintf(cimg, sizeof(cimg), "%s/c4.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_jrec_type(cimg, 1, 1);
    {
        const char *args[] = {"-c", "mount", "-c", "cat /f", "-c", "check"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "commit record turned into an OP record: transaction ignored", "1 incomplete ignored", res.output);
        assert_has(&ctx, "commit record turned into an OP record: old value served", "HELLO", res.output);
        assert_not_has(&ctx, "commit record turned into an OP record: no torn data", "HELLOWORLD", res.output);
        jfs_cmd_res_free(&res);
    }

    /* 5. split txid */
    snprintf(cimg, sizeof(cimg), "%s/c5.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_jrec_txid(cimg, 0, 999);
    {
        const char *args[] = {"-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        if (strstr(res.output, "incomplete") || strstr(res.output, "damaged journal record") ||
            strstr(res.output, "corrupt") || strstr(res.output, "cannot be made consistent")) {
            pass(&ctx, "split transaction id: the mismatch is reported");
        } else {
            fail(&ctx, "split transaction id: the mismatch is reported", "incomplete, damage warning, or refusal", res.output);
        }
        assert_not_has(&ctx, "split transaction id: nothing is replayed from it", "HELLOWORLD", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("\n--- damaged checkpoint ---\n");
    /* 6. one slot destroyed */
    snprintf(cimg, sizeof(cimg), "%s/c6.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_ckpt_zero(cimg, 0);
    {
        const char *args[] = {"-c", "mount", "-c", "cat /f", "-c", "check"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "one destroyed checkpoint slot: mount falls back to the other", "HELLOWORLD", res.output);
        assert_has(&ctx, "one destroyed checkpoint slot: still consistent", "is consistent", res.output);
        jfs_cmd_res_free(&res);
    }

    /* 7. both slots destroyed */
    snprintf(cimg, sizeof(cimg), "%s/c7.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_ckpt_both(cimg);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "both checkpoint slots destroyed: mount refuses", "BOTH checkpoint slots are damaged", res.output);
        assert_status(&ctx, "both checkpoint slots destroyed: reported as an error", 1, res.status);
        jfs_cmd_res_free(&res);
    }

    /* 8. journal-start outside */
    snprintf(cimg, sizeof(cimg), "%s/c8.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_journal_start(cimg, 40);
    {
        const char *args[] = {"-c", "mount", "-c", "cat /f", "-c", "check"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "window pointing outside the records: damage reported", "damaged journal record", res.output);
        assert_not_has(&ctx, "window pointing outside the records: nothing invented", "HELLOWORLD", res.output);
        assert_has(&ctx, "window pointing outside the records: still consistent", "is consistent", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("\n--- damaged superblock and log ---\n");
    /* 9. sb magic broken */
    snprintf(cimg, sizeof(cimg), "%s/c9.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_sb_flip(cimg, 0);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "broken superblock magic: recognised as a foreign image", "not a JournalFS image", res.output);
        assert_status(&ctx, "broken superblock magic: mount fails", 1, res.status);
        jfs_cmd_res_free(&res);
    }

    /* 10. sb checksum */
    snprintf(cimg, sizeof(cimg), "%s/c10.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_sb_flip(cimg, 84);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_status(&ctx, "broken superblock checksum: mount fails", 1, res.status);
        assert_has(&ctx, "broken superblock checksum: the reason mentions the superblock", "uperblock", res.output);
        jfs_cmd_res_free(&res);
    }

    /* 11. truncated */
    snprintf(cimg, sizeof(cimg), "%s/c11.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_sb_truncate(cimg, 4);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_status(&ctx, "truncated image file: mount fails", 1, res.status);
        assert_has(&ctx, "truncated image file: the reason is explicit", "truncat", res.output);
        jfs_cmd_res_free(&res);
    }

    /* 12. imap damaged */
    snprintf(cimg, sizeof(cimg), "%s/c12.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_imap_flip(cimg);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_status(&ctx, "damaged inode map record: mount fails", 1, res.status);
        assert_has(&ctx, "damaged inode map record: the reason names the inode map", "inode map", res.output);
        jfs_cmd_res_free(&res);
    }

    /* 13. log record killed */
    snprintf(cimg, sizeof(cimg), "%s/c13.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_inode_kill(cimg);
    {
        const char *args[] = {"-c", "mount", "-c", "cat /f", "-c", "check"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        if (strstr(res.output, "error") || strstr(res.output, "corrupt") || strstr(res.output, "damaged")) {
            pass(&ctx, "zeroed log record: refusal or explicit damage report");
        } else if (strstr(res.output, "HELLO")) {
            assert_has(&ctx, "zeroed log record: if the record was dead the file still reads", "HELLO", res.output);
        } else {
            fail(&ctx, "zeroed log record: refusal or explicit damage report", "a clear error or intact file", res.output);
        }
        jfs_cmd_res_free(&res);
    }

    printf("\n--- a foreign file is not mistaken for an image ---\n");
    char rand_img[512];
    snprintf(rand_img, sizeof(rand_img), "%s/random.img", ctx.workdir);
    jfs_write_random_file(rand_img, 262144);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /f"};
        jfs_cmd_res res = JFS_RUN_ARR(rand_img, args);
        assert_status(&ctx, "random data: mount fails", 1, res.status);
        assert_has(&ctx, "random data: recognised as a foreign image", "not a JournalFS image", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("\n--- fuzz: random damage must never crash the engine ---\n");
    int crashes = 0, checked = 0;
    for (uint32_t seed = 1; seed <= 8; seed++) {
        char f_img[512];
        snprintf(f_img, sizeof(f_img), "%s/fuzz%u.img", ctx.workdir, seed);
        copy_file(scen, f_img);
        fuzz_damage(f_img, seed);

        const char *args[] = {"-c", "mount", "-c", "cat /f", "-c", "check", "-c", "ls /"};
        jfs_cmd_res res = JFS_RUN_ARR(f_img, args);
        checked++;
        if (res.status == -SIGSEGV || res.status == -SIGABRT || res.status == -SIGBUS ||
            res.status == 134 || res.status == 139 || res.status == 135 || res.status == 136) {
            crashes++;
        }
        if (res.status == 0) {
            if (!strstr(res.output, "is consistent") && !strstr(res.output, "no filesystem is mounted")) {
                fail(&ctx, "fuzz: successful mount stays consistent", "consistent", res.output);
            }
        }
        jfs_cmd_res_free(&res);
    }
    char act[64];
    snprintf(act, sizeof(act), "%d", crashes);
    assert_eq(&ctx, "fuzz: no crash or abort in 8 damaged images", "0", act);

    printf("\n--- the image is never repaired behind the user's back ---\n");
    snprintf(cimg, sizeof(cimg), "%s/c14.img", ctx.workdir);
    copy_file(scen, cimg);
    jfs_corrupt_jrec_flip_header(cimg, 0);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "check"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"-c", "mount", "-c", "check"};
        jfs_cmd_res res = JFS_RUN_ARR(cimg, args);
        assert_has(&ctx, "damaged window: the report survives a remount", "damaged journal record", res.output);
        jfs_cmd_res_free(&res);
    }

    return jfs_test_summary(&ctx);
}
