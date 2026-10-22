/*
 * test_errors.c -- error handling tests in C11.
 *
 * Every case must fail with a clear message, a non-zero exit status and no
 * crash (the CLI must never segfault on malformed input).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_framework.h"

int main(void)
{
    jfs_test_ctx ctx;
    jfs_test_init(&ctx, "test_errors");

    char img[512];
    snprintf(img, sizeof(img), "%s/err.img", ctx.workdir);

    {
        const char *args[] = {"-c", "format --size 1m"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /exists", "-c", "write /exists data"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }

    printf("--- file operation errors ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /missing"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "read a nonexistent file", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /exists"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "create a duplicate name", "already exists", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "write /missing text"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "write to a nonexistent file", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "append /missing text"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "append to a nonexistent file", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rm /missing"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "delete a nonexistent file", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rmdir /missing"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rmdir a nonexistent directory", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "stat /missing"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "stat a nonexistent file", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "truncate /missing 4"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "truncate a nonexistent file", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rename /missing /x"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rename a nonexistent file", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "mkdir /exists"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "mkdir over an existing name", "already exists", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /exists/child"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "path through a file", "not a directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rmdir /exists"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rmdir on a file", "not a directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cat /exists"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "cat on a regular file is fine", "data", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "mkdir /d1", "-c", "cat /d1"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "cat on a directory", "is a directory", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- malformed arguments ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "bogus"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "unknown command", "unknown command", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "create without an argument", "missing file name", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "truncate /exists abc"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "truncate with a non numeric size", "bad size", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "chmod 999 /exists"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "chmod with an invalid mode", "bad mode", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "chmod 644"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "chmod without a file", "usage: chmod", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rename /exists"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rename with one argument", "usage: rename", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"-c", "format --size banana"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "format with a bad size", "bad size", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /this-name-is-far-too-long-for-journalfs"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "name longer than JFS_NAME_MAX", "file name too long", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /bad/name/here"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "path whose parent does not exist", "no such file or directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /exists/child"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "component that is not a directory", "not a directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        char longname[350];
        memset(longname, 'n', 300);
        longname[300] = '\0';
        char cmd_buf[400];
        snprintf(cmd_buf, sizeof(cmd_buf), "create /%s", longname);
        const char *args[] = {"--quiet", "-c", "mount", "-c", cmd_buf};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "very long path component", "file name too long", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--crash", "nosuchpoint", "-c", "mount"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "unknown crash point", "unknown crash point", res.output);
        assert_status(&ctx, "unknown crash point exit status", 2, res.status);
        jfs_cmd_res_free(&res);
    }

    printf("--- limits ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "fill /exists 64769 x"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "file over the maximum size", "exceeds the maximum file size", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "fill /exists 1t x"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "absurd size", "bad size", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- format/mount argument errors ---\n");
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/tiny.img", ctx.workdir);
        const char *args[] = {"-c", "format --size 16k"};
        jfs_cmd_res res = JFS_RUN_ARR(path, args);
        assert_has(&ctx, "image smaller than the minimum", "at least 1024 KiB", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/short.img", ctx.workdir);
        const char *args[] = {"-c", "format --size 1000"};
        jfs_cmd_res res = JFS_RUN_ARR(path, args);
        assert_has(&ctx, "image size that is not a block multiple", "multiple of", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/absent.img", ctx.workdir);
        const char *args[] = {"-c", "mount"};
        jfs_cmd_res res = JFS_RUN_ARR(path, args);
        assert_has(&ctx, "mount a missing image", "cannot open image", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/text.img", ctx.workdir);
        FILE *fp = fopen(path, "w");
        if (fp) {
            fprintf(fp, "this is not a filesystem image, just ordinary text padding it out");
            uint8_t zero[4096] = {0};
            fwrite(zero, 1, sizeof(zero), fp);
            fclose(fp);
        }
        const char *args[] = {"-c", "mount"};
        jfs_cmd_res res = JFS_RUN_ARR(path, args);
        assert_has(&ctx, "mount a file that is not an image", "not a JournalFS image", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/trunc.img", ctx.workdir);
        FILE *fp = fopen(path, "w");
        if (fp) {
            uint8_t zero[100] = {0};
            fwrite(zero, 1, sizeof(zero), fp);
            fclose(fp);
        }
        const char *args[] = {"-c", "mount"};
        jfs_cmd_res res = JFS_RUN_ARR(path, args);
        assert_has(&ctx, "mount a truncated image", "smaller than one block", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /a", "-c", "rmdir /a"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rmdir on a regular file", "not a directory", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- commands without a mounted filesystem ---\n");
    {
        char nomount[512];
        snprintf(nomount, sizeof(nomount), "%s/nomount.img", ctx.workdir);
        const char *unmounted_cmds[] = {
            "create /x", "cat /x", "ls", "df", "sync", "check", "journal", "stats", "stat /x", "rm /x"
        };
        for (size_t i = 0; i < sizeof(unmounted_cmds)/sizeof(unmounted_cmds[0]); i++) {
            char desc[128];
            snprintf(desc, sizeof(desc), "unmounted filesystem rejects '%s'", unmounted_cmds[i]);
            const char *args[] = {"-c", unmounted_cmds[i]};
            jfs_cmd_res res = JFS_RUN_ARR(nomount, args);
            assert_has(&ctx, desc, "no filesystem is mounted", res.output);
            jfs_cmd_res_free(&res);
        }
    }

    printf("--- no crash on hostile input ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "write /exists \"unterminated quote"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_status(&ctx, "unterminated quote does not crash the CLI", 0, res.status);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "///////"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_status(&ctx, "path of only slashes", 1, res.status);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /.."};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_status(&ctx, "dotdot name is refused", 1, res.status);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "cd ..", "-c", "pwd"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "cd .. from the root stays at the root", "/", res.output);
        jfs_cmd_res_free(&res);
    }

    return jfs_test_summary(&ctx);
}
