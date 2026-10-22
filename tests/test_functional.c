/*
 * test_functional.c -- milestone 1 functional tests in C11.
 *
 * Covers: format, mount, unmount, create, write, append, read, cat, ls, stat,
 * truncate, rename, rm, mkdir, cd, pwd, rmdir, chmod, df, sync, hash, the
 * documented size limits and the documented metadata behaviour.
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
    jfs_test_init(&ctx, "test_functional");

    char img[512];
    snprintf(img, sizeof(img), "%s/func.img", ctx.workdir);

    printf("--- formatting and mounting ---\n");
    {
        const char *args[] = {"-c", "format --size 1m"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "format creates an image", "formatted", res.output);
        jfs_cmd_res_free(&res);

        if (access(img, F_OK) == 0)
            pass(&ctx, "image file exists");
        else
            fail(&ctx, "image file exists", "a file", "missing");
    }
    {
        const char *args[] = {"-c", "mount", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "mount succeeds on a fresh image", "mounted", res.output);
        assert_has(&ctx, "unmount succeeds", "unmounted", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"-c", "format --size 1m"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "format refuses to overwrite without --force", "already exists", res.output);
        assert_status(&ctx, "format failure exit status", 1, res.status);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"-c", "format --size 1m --force"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "format --force overwrites", "formatted", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- file basics ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /hello", "-c", "write /hello HELLO",
                              "-c", "cat /hello", "-c", "read /hello", "-c", "stat /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "create reports success", "created /hello", res.output);
        assert_has(&ctx, "write reports success", "wrote /hello", res.output);
        assert_has(&ctx, "cat returns the content", "HELLO", res.output);
        assert_has(&ctx, "read prints a size header", "5 bytes in /hello", res.output);
        assert_has(&ctx, "stat shows the inode number", "Inode: 2", res.output);
        assert_has(&ctx, "stat shows the mode", "-rw-r--r-- (0644)", res.output);
        assert_has(&ctx, "stat shows a log address", "log address 0x", res.output);
        assert_has(&ctx, "stat shows the size", "Size: 5", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "append /hello WORLD", "-c", "cat /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "append extends the content", "HELLOWORLD", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "write /hello \"line1\\nline2\"", "-c", "cat /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "write with escapes produces two lines", "line1", res.output);
        assert_has(&ctx, "second line present", "line2", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "write /hello JOURNALFS", "-c", "hash /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "hash of the new content is reported", "crc32c 0x", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "truncate /hello 4", "-c", "cat /hello", "-c", "stat /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "truncate cuts the content", "JOUR", res.output);
        assert_has(&ctx, "stat shows the truncated size", "Size: 4", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "truncate /hello 8", "-c", "hash /hello"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "truncate can grow a file with zeros", "8 byte(s)", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- listing, renaming, deleting ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /other", "-c", "ls"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "ls lists both files", "hello", res.output);
        assert_has(&ctx, "ls lists the second file", "other", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rename /hello /renamed", "-c", "ls"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rename reports success", "renamed /hello -> /renamed", res.output);
        assert_has(&ctx, "renamed file appears in ls", "renamed", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rm /renamed"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rm reports success", "removed /renamed", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "ls"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_not_has(&ctx, "removed file is gone from ls", "renamed", res.output);
        assert_has(&ctx, "only one entry remains", "1 entry", res.output);
        jfs_cmd_res_free(&res);
        assert_consistent(&ctx, "filesystem is consistent after rename/rm", img);
    }

    printf("--- directories ---\n");
    {
        const char *args[] = {
            "--quiet", "-c", "mount",
            "-c", "mkdir /docs",
            "-c", "mkdir /docs/sub",
            "-c", "create /docs/a.txt",
            "-c", "write /docs/a.txt alpha",
            "-c", "create /docs/sub/b.txt",
            "-c", "write /docs/sub/b.txt beta",
            "-c", "cd /docs/sub",
            "-c", "pwd",
            "-c", "cat b.txt",
            "-c", "cd ..",
            "-c", "ls",
            "-c", "ls /docs"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "mkdir reports success", "created directory /docs", res.output);
        assert_has(&ctx, "nested mkdir reports success", "created directory /docs/sub", res.output);
        assert_has(&ctx, "cd changes the working directory", "/docs/sub", res.output);
        assert_has(&ctx, "relative path reads through cd", "beta", res.output);
        assert_has(&ctx, "cd .. moves back up", "a.txt", res.output);
        assert_has(&ctx, "ls PATH lists another directory", "sub", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "stat /docs"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "stat marks directories", "Type: directory", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rmdir /docs"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rmdir refuses a non-empty directory", "not empty", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "rm -r /docs"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "rm -r removes a tree", "removed /docs", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "ls"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_not_has(&ctx, "removed tree is gone from ls", "docs", res.output);
        assert_has(&ctx, "only the untouched file remains", "1 entry", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- permissions (simulated, single user) ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /secret", "-c", "write /secret top",
                              "-c", "chmod 000 /secret", "-c", "cat /secret"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "chmod applies", "00", res.output);
        assert_has(&ctx, "read without permission is denied", "permission denied", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "chmod 400 /secret", "-c", "cat /secret"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "read permission restored", "top", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "chmod 444 /secret", "-c", "write /secret nope"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "write without permission is denied", "permission denied", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "mkdir /ro", "-c", "chmod 555 /ro", "-c", "create /ro/x"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "create in a write-protected directory is denied", "permission denied", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- limits and capacity reporting ---\n");
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /big", "-c", "fill /big 64768 z", "-c", "stat /big"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "maximum file size is accepted", "Size: 64768", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "fill /big 64769 z"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "one byte over the limit is refused", "exceeds the maximum file size", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "df"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "df reports capacity", "total", res.output);
        assert_has(&ctx, "df reports inodes", "inodes:", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "sync", "-c", "check", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "sync writes a checkpoint", "synced: checkpoint seq", res.output);
        assert_has(&ctx, "check reports a consistent filesystem", "is consistent", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("--- help and exit ---\n");
    {
        const char *args[] = {"--help"};
        jfs_cmd_res res = JFS_RUN_ARR(NULL, args);
        assert_has(&ctx, "help lists commands", "write FILE TEXT", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--version"};
        jfs_cmd_res res = JFS_RUN_ARR(NULL, args);
        assert_has(&ctx, "version reports the format version", "format version 1", res.output);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "help stat", "-c", "exit"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        assert_has(&ctx, "help for one command", "usage: stat FILE", res.output);
        assert_status(&ctx, "clean run exit status", 0, res.status);
        jfs_cmd_res_free(&res);
    }

    return jfs_test_summary(&ctx);
}
