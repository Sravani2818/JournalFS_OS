/*
 * test_stress.c -- randomized workload testing in C11 (runs build/fsstress).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_framework.h"

static const char *get_stress_bin(void)
{
    const char *env = getenv("JFS_FSSTRESS");
    if (env && *env) return env;
    if (access("build/fsstress", X_OK) == 0) return "build/fsstress";
    if (access("../build/fsstress", X_OK) == 0) return "../build/fsstress";
    return "build/fsstress";
}

static jfs_cmd_res run_fsstress(const char *const args[], size_t n_args)
{
    const char *bin = get_stress_bin();
    size_t total = 1 + n_args + 1;
    char **argv = (char **)calloc(total, sizeof(char *));
    size_t idx = 0;
    argv[idx++] = (char *)bin;
    for (size_t i = 0; i < n_args; i++) {
        argv[idx++] = (char *)args[i];
    }
    argv[idx] = NULL;
    jfs_cmd_res res = jfs_exec(argv);
    free(argv);
    return res;
}

int main(void)
{
    jfs_test_ctx ctx;
    jfs_test_init(&ctx, "test_stress");

    printf("--- steady state (2 MiB image, 600 operations per run) ---\n");
    int seeds[] = {1, 7, 12, 33};
    for (int i = 0; i < 4; i++) {
        char img[512], seed_str[16];
        snprintf(img, sizeof(img), "%s/s_steady_%d.img", ctx.workdir, seeds[i]);
        snprintf(seed_str, sizeof(seed_str), "%d", seeds[i]);

        const char *args[] = {"--image", img, "--size", "2", "--seed", seed_str, "--ops", "600", "--quiet"};
        jfs_cmd_res res = run_fsstress(args, 9);
        char desc[128];
        snprintf(desc, sizeof(desc), "seed %d: 600 operations pass", seeds[i]);
        assert_status(&ctx, desc, 0, res.status);
        jfs_cmd_res_free(&res);
    }

    printf("\n--- restart in the middle of a workload ---\n");
    char r_img[512];
    snprintf(r_img, sizeof(r_img), "%s/s_restart.img", ctx.workdir);
    {
        const char *args[] = {"--image", r_img, "--size", "2", "--seed", "5", "--ops", "400", "--quiet"};
        jfs_cmd_res res = run_fsstress(args, 9);
        assert_status(&ctx, "first run formats and fills the image", 0, res.status);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--image", r_img, "--adopt", "--seed", "6", "--ops", "400", "--quiet"};
        jfs_cmd_res res = run_fsstress(args, 8);
        assert_status(&ctx, "second run adopts the existing contents and continues", 0, res.status);
        assert_has(&ctx, "the second run really did adopt files", "file(s) verified", res.output);
        jfs_cmd_res_free(&res);
        assert_consistent(&ctx, "the image is consistent after two workloads", r_img);
    }

    printf("\n--- a workload that crashes and continues ---\n");
    const char *points[] = {
        "post_write", "post_sync", "post_commit", "post_commit_sync", "post_apply",
        "checkpoint_post", "compact_post"
    };
    for (int i = 0; i < 7; i++) {
        const char *p = points[i];
        char img[512];
        snprintf(img, sizeof(img), "%s/s_%s.img", ctx.workdir, p);

        const char *args1[] = {"--image", img, "--size", "2", "--seed", "21", "--ops", "300",
                               "--crash-after", "120", "--crash-point", p, "--quiet"};
        jfs_cmd_res res1 = run_fsstress(args1, sizeof(args1) / sizeof(args1[0]));
        char desc[128];
        snprintf(desc, sizeof(desc), "crash at %s: the workload was aborted mid-flight", p);
        assert_status(&ctx, desc, 99, res1.status);
        jfs_cmd_res_free(&res1);

        const char *args2[] = {"--image", img, "--adopt", "--seed", "22", "--ops", "300", "--quiet"};
        jfs_cmd_res res2 = run_fsstress(args2, 8);
        snprintf(desc, sizeof(desc), "crash at %s: recovery and a new workload both succeed", p);
        assert_status(&ctx, desc, 0, res2.status);
        jfs_cmd_res_free(&res2);

        snprintf(desc, sizeof(desc), "crash at %s: the recovered image is consistent", p);
        assert_consistent(&ctx, desc, img);
    }

    printf("\n--- crash in the middle of the recovery run itself ---\n");
    char rec_img[512];
    snprintf(rec_img, sizeof(rec_img), "%s/s_recover.img", ctx.workdir);
    {
        const char *args[] = {"--image", rec_img, "--size", "2", "--seed", "31", "--ops", "200",
                              "--crash-after", "80", "--crash-point", "post_commit_sync", "--quiet"};
        jfs_cmd_res res = run_fsstress(args, sizeof(args) / sizeof(args[0]));
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--image", rec_img, "--adopt", "--seed", "32", "--ops", "200",
                              "--crash-after", "40", "--crash-point", "checkpoint_mid", "--quiet"};
        jfs_cmd_res res = run_fsstress(args, sizeof(args) / sizeof(args[0]));
        assert_status(&ctx, "crash during the recovery run: aborted as asked", 99, res.status);
        jfs_cmd_res_free(&res);
    }
    {
        const char *args[] = {"--image", rec_img, "--adopt", "--seed", "33", "--ops", "200", "--quiet"};
        jfs_cmd_res res = run_fsstress(args, sizeof(args) / sizeof(args[0]));
        assert_status(&ctx, "the image still works after two crashes", 0, res.status);
        jfs_cmd_res_free(&res);
        assert_consistent(&ctx, "two crashes left a consistent image", rec_img);
    }

    printf("\n--- a completely full filesystem ---\n");
    char full_img[512];
    snprintf(full_img, sizeof(full_img), "%s/s_full.img", ctx.workdir);
    {
        const char *args[] = {"--image", full_img, "--size", "1", "--seed", "12", "--ops", "700"};
        jfs_cmd_res res = run_fsstress(args, 8);
        assert_status(&ctx, "1 MiB image: the workload runs to the end", 0, res.status);
        assert_has(&ctx, "1 MiB image: the filesystem refused work instead of failing", "clean error(s)", res.output);
        pass(&ctx, "1 MiB image: operation(s) were refused cleanly");
        jfs_cmd_res_free(&res);
        assert_consistent(&ctx, "a full filesystem is still consistent", full_img);
    }
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "ls /"};
        jfs_cmd_res res = JFS_RUN_ARR(full_img, args);
        assert_has(&ctx, "a full filesystem still lists its files", "entries", res.output);
        jfs_cmd_res_free(&res);
    }

    printf("\n--- determinism ---\n");
    char det1_img[512], det2_img[512];
    snprintf(det1_img, sizeof(det1_img), "%s/s_det1.img", ctx.workdir);
    snprintf(det2_img, sizeof(det2_img), "%s/s_det2.img", ctx.workdir);
    {
        const char *args1[] = {"--image", det1_img, "--size", "2", "--seed", "99", "--ops", "300", "--quiet"};
        jfs_cmd_res res1 = run_fsstress(args1, 9);
        const char *args2[] = {"--image", det2_img, "--size", "2", "--seed", "99", "--ops", "300", "--quiet"};
        jfs_cmd_res res2 = run_fsstress(args2, 9);
        assert_eq(&ctx, "the same seed produces the same workload", res1.output, res2.output);
        jfs_cmd_res_free(&res1);
        jfs_cmd_res_free(&res2);
    }

    printf("\n--- the image is left clean ---\n");
    char final_img[512];
    snprintf(final_img, sizeof(final_img), "%s/s_final.img", ctx.workdir);
    {
        const char *args[] = {"--image", final_img, "--size", "2", "--seed", "44", "--ops", "500", "--quiet"};
        jfs_cmd_res res = run_fsstress(args, 9);
        jfs_cmd_res_free(&res);
    }
    {
        char cmd_buf[550];
        snprintf(cmd_buf, sizeof(cmd_buf), "inspect %s", final_img);
        const char *args[] = {"-c", cmd_buf};
        jfs_cmd_res res = JFS_RUN_ARR(NULL, args);
        assert_has(&ctx, "clean unmount: the image is marked clean", "CLEAN", res.output);
        assert_has(&ctx, "clean unmount: the journal window was reclaimed", "used=0 of", res.output);
        jfs_cmd_res_free(&res);
        assert_consistent(&ctx, "the image mounts and checks out", final_img);
    }

    return jfs_test_summary(&ctx);
}
