/*
 * test_comparison.c -- C11 automated test suite for Add-On 1 (Group Commit)
 * and Add-On 2 (Failure Mode Comparison: Without-Journal vs. With-Journal).
 *
 * Demonstrates:
 *   1. Without-Journal Failure Modes:
 *      - Lack of atomicity: intermediate or uncommitted writes become exposed.
 *      - Vulnerability to crashes: without WAL replay, torn updates cannot be restored.
 *   2. With-Journal Crash Recovery:
 *      - Pre-commit crashes: partial changes discarded cleanly, old state wins.
 *      - Post-commit crashes: recovery scans journal, validates CRC32C, replays forward.
 *   3. Group Commit:
 *      - Batching transactions (e.g. batch=4) reduces sync barriers.
 *      - Stats verification and crash recovery under group commit.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

#include "test_framework.h"

static void setup_test_image(const char *img)
{
    unlink(img);
    char fmt[256];
    snprintf(fmt, sizeof(fmt), "format %s --size 2m --force", img);
    const char *args[] = {"-q", "-c", fmt};
    jfs_cmd_res res = JFS_RUN_ARR(NULL, args);
    jfs_cmd_res_free(&res);
}

int main(void)
{
    jfs_test_ctx ctx;
    jfs_test_init(&ctx, "test_comparison");

    char img[512];
    snprintf(img, sizeof(img), "%s/comparison.img", ctx.workdir);

    printf("--- Failure Mode Comparison: Without-Journal vs With-Journal ---\n");

    /* ------------------------------------------------------------- */
    /* Scenario 1: Pre-commit crash with Journaling                  */
    /* ------------------------------------------------------------- */
    {
        setup_test_image(img);

        /* Initial baseline: file /state holds INITIAL */
        const char *args_init[] = {
            "--quiet", "-c", "mount", "-c", "create /state", "-c", "write /state INITIAL", "-c", "unmount"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args_init);
        assert_status(&ctx, "with-journal: baseline initialized", 0, res.status);
        jfs_cmd_res_free(&res);

        /* Crash before commit record sync: append MUTATED with crash at post_write */
        const char *args_crash[] = {
            "--quiet", "--crash", "post_write",
            "-c", "mount", "-c", "write /state MUTATED"
        };
        jfs_cmd_res res_crash = JFS_RUN_ARR(img, args_crash);
        assert_status(&ctx, "with-journal: pre-commit crash injected", 99, res_crash.status);
        jfs_cmd_res_free(&res_crash);

        /* Re-mount: verify journal discards uncommitted change; old value preserved */
        const char *args_check[] = {
            "--quiet", "-c", "mount", "-c", "cat /state", "-c", "check", "-c", "unmount"
        };
        jfs_cmd_res res_chk = JFS_RUN_ARR(img, args_check);
        assert_status(&ctx, "with-journal: mount after pre-commit crash succeeds", 0, res_chk.status);
        assert_has(&ctx, "with-journal: uncommitted write discarded (old value wins)", "INITIAL", res_chk.output);
        assert_not_has(&ctx, "with-journal: partial mutated data is not exposed", "MUTATED", res_chk.output);
        assert_has(&ctx, "with-journal: filesystem is consistent", "is consistent", res_chk.output);
        jfs_cmd_res_free(&res_chk);
    }

    /* ------------------------------------------------------------- */
    /* Scenario 2: Post-commit crash with Journaling                 */
    /* ------------------------------------------------------------- */
    {
        setup_test_image(img);

        const char *args_init[] = {
            "--quiet", "-c", "mount", "-c", "create /state", "-c", "write /state INITIAL", "-c", "unmount"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args_init);
        jfs_cmd_res_free(&res);

        /* Crash after commit record synced: durable commit, not yet applied to log */
        const char *args_crash[] = {
            "--quiet", "--crash", "post_commit_sync",
            "-c", "mount", "-c", "append /state _COMMITTED"
        };
        jfs_cmd_res res_crash = JFS_RUN_ARR(img, args_crash);
        assert_status(&ctx, "with-journal: post-commit crash injected", 99, res_crash.status);
        jfs_cmd_res_free(&res_crash);

        /* Re-mount: verify replay recovers committed transaction forward */
        const char *args_check[] = {
            "-v", "-c", "mount", "-c", "cat /state", "-c", "check", "-c", "unmount"
        };
        jfs_cmd_res res_chk = JFS_RUN_ARR(img, args_check);
        assert_status(&ctx, "with-journal: mount after post-commit crash succeeds", 0, res_chk.status);
        assert_has(&ctx, "with-journal: committed write replayed forward", "INITIAL_COMMITTED", res_chk.output);
        assert_has(&ctx, "with-journal: recovery reports replayed transaction", "1 committed and replayed", res_chk.output);
        assert_has(&ctx, "with-journal: filesystem is consistent", "is consistent", res_chk.output);
        jfs_cmd_res_free(&res_chk);
    }

    /* ------------------------------------------------------------- */
    /* Scenario 3: Without-Journal Failure Modes (--no-journal)     */
    /* ------------------------------------------------------------- */
    {
        setup_test_image(img);

        /* Initial baseline without journal */
        const char *args_init[] = {
            "--no-journal", "-c", "mount", "-c", "create /base", "-c", "write /base SAFE", "-c", "unmount"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args_init);
        assert_has(&ctx, "without-journal: warns that WAL is disabled", "logging is DISABLED", res.output);
        jfs_cmd_res_free(&res);

        /* In --no-journal mode, without write-ahead logging,
         * a crash during checkpoint update cannot be replayed from WAL. */
        const char *args_crash[] = {
            "--no-journal", "--crash", "checkpoint_mid",
            "-c", "mount", "-c", "create /lost_file"
        };
        jfs_cmd_res res_crash = JFS_RUN_ARR(img, args_crash);
        assert_status(&ctx, "without-journal: crash interrupted uncheckpointed session", 99, res_crash.status);
        jfs_cmd_res_free(&res_crash);

        /* Re-mount: verify recovery has no journal window to rebuild state */
        const char *args_check[] = {
            "-c", "mount", "-c", "ls", "-c", "unmount"
        };
        jfs_cmd_res res_chk = JFS_RUN_ARR(img, args_check);
        assert_has(&ctx, "without-journal: recovery reports nothing to redo", "nothing to redo", res_chk.output);
        assert_not_has(&ctx, "without-journal: uncheckpointed file was not recovered", "lost_file", res_chk.output);
        jfs_cmd_res_free(&res_chk);
    }

    printf("\n--- Add-On 1: Group Commit Verification ---\n");

    /* ------------------------------------------------------------- */
    /* Scenario 4: Group Commit Functionality & Batching             */
    /* ------------------------------------------------------------- */
    {
        setup_test_image(img);

        /* Set group-commit batch to 4 */
        const char *args_gc[] = {
            "--quiet", "--group-commit", "4",
            "-c", "mount",
            "-c", "create /f1", "-c", "write /f1 111",
            "-c", "create /f2", "-c", "write /f2 222",
            "-c", "create /f3", "-c", "write /f3 333",
            "-c", "create /f4", "-c", "write /f4 444",
            "-c", "stats",
            "-c", "unmount"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args_gc);
        assert_status(&ctx, "group-commit: 4-transaction batch runs cleanly", 0, res.status);
        assert_has(&ctx, "group-commit: stats report group_commit_batch=4", "group_commit_batch,4", res.output);
        assert_has(&ctx, "group-commit: stats report group commits occurred", "group_commits,", res.output);
        assert_has(&ctx, "group-commit: stats report batched flushes occurred", "group_commit_flushes,", res.output);
        jfs_cmd_res_free(&res);

        /* Verify all 4 files are intact and consistent after unmount flush */
        const char *args_verify[] = {
            "--quiet", "-c", "mount",
            "-c", "cat /f1", "-c", "cat /f2", "-c", "cat /f3", "-c", "cat /f4",
            "-c", "check", "-c", "unmount"
        };
        jfs_cmd_res res_ver = JFS_RUN_ARR(img, args_verify);
        assert_has(&ctx, "group-commit: file 1 persisted", "111", res_ver.output);
        assert_has(&ctx, "group-commit: file 2 persisted", "222", res_ver.output);
        assert_has(&ctx, "group-commit: file 3 persisted", "333", res_ver.output);
        assert_has(&ctx, "group-commit: file 4 persisted", "444", res_ver.output);
        assert_has(&ctx, "group-commit: filesystem remains consistent", "is consistent", res_ver.output);
        jfs_cmd_res_free(&res_ver);
    }

    /* ------------------------------------------------------------- */
    /* Scenario 5: Dynamic 'set group-commit' CLI command            */
    /* ------------------------------------------------------------- */
    {
        setup_test_image(img);

        const char *args_set[] = {
            "-c", "mount",
            "-c", "set group-commit 8",
            "-c", "stats",
            "-c", "unmount"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args_set);
        assert_status(&ctx, "set group-commit command succeeds", 0, res.status);
        assert_has(&ctx, "set group-commit acknowledged", "group-commit = 8 transaction(s)", res.output);
        assert_has(&ctx, "stats confirms group_commit_batch is 8", "group_commit_batch,8", res.output);
        jfs_cmd_res_free(&res);
    }

    return jfs_test_summary(&ctx);
}
