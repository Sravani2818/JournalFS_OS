/*
 * test_framework.h -- Common testing harness for JournalFS C11 test suites.
 *
 * Provides:
 *   - Process execution and output capture (jfs_run, jfs_run_m, jfs_raw)
 *   - Assertion helpers (assert_eq, assert_has, assert_not_has, assert_status, etc.)
 *   - Test suite logging to results/<name>.log and console reporting
 *   - Subprocess execution without using shell scripts
 */
#ifndef JFS_TEST_FRAMEWORK_H
#define JFS_TEST_FRAMEWORK_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <ftw.h>

typedef struct {
    char   *output;
    size_t  length;
    int     status;     /* exit code or -signal */
} jfs_cmd_res;

typedef struct {
    const char *name;
    char        logfile[256];
    char        workdir[256];
    FILE       *log_fp;
    int         pass_count;
    int         fail_count;
} jfs_test_ctx;

static inline void jfs_cmd_res_free(jfs_cmd_res *res)
{
    if (res && res->output) {
        free(res->output);
        res->output = NULL;
        res->length = 0;
        res->status = 0;
    }
}

/* Locate the journalfs binary */
static inline const char *jfs_get_bin(void)
{
    const char *env = getenv("JFS_BIN");
    if (env && *env)
        return env;
    if (access("./journalfs", X_OK) == 0)
        return "./journalfs";
    if (access("build/journalfs", X_OK) == 0)
        return "build/journalfs";
    if (access("../build/journalfs", X_OK) == 0)
        return "../build/journalfs";
    return "./journalfs";
}

/* Initialize test context, log file, and temp workspace */
static inline int jfs_test_init(jfs_test_ctx *ctx, const char *suite_name)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->name = suite_name;

    const char *res_dir = getenv("JFS_RESULTS");
    if (!res_dir || !*res_dir) {
        if (access("results", F_OK) == 0)
            res_dir = "results";
        else if (access("../results", F_OK) == 0)
            res_dir = "../results";
        else {
            mkdir("results", 0755);
            res_dir = "results";
        }
    } else {
        mkdir(res_dir, 0755);
    }

    snprintf(ctx->logfile, sizeof(ctx->logfile), "%s/%s.log", res_dir, suite_name);
    ctx->log_fp = fopen(ctx->logfile, "w");
    if (!ctx->log_fp) {
        fprintf(stderr, "warning: cannot open log file %s: %s\n", ctx->logfile, strerror(errno));
    }

    /* Temporary working directory */
    char tmpl[512];
    snprintf(tmpl, sizeof(tmpl), "/tmp/jfs_test_%s_XXXXXX", suite_name);
    char *dir = mkdtemp(tmpl);
    if (dir) {
        snprintf(ctx->workdir, sizeof(ctx->workdir), "%s", dir);
    } else {
        snprintf(ctx->workdir, sizeof(ctx->workdir), "/tmp");
    }

    if (ctx->log_fp) {
        fprintf(ctx->log_fp, "=== %s ===\n", suite_name);
        fprintf(ctx->log_fp, "binary : %s\n", jfs_get_bin());
        fprintf(ctx->log_fp, "workdir: %s\n\n", ctx->workdir);
        fflush(ctx->log_fp);
    }
    return 0;
}

static inline int jfs_unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb; (void)typeflag; (void)ftwbuf;
    return remove(fpath);
}

static inline void jfs_test_cleanup(jfs_test_ctx *ctx)
{
    if (ctx->workdir[0] && strncmp(ctx->workdir, "/tmp/jfs_test_", 14) == 0) {
        nftw(ctx->workdir, jfs_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
    }
    if (ctx->log_fp) {
        fclose(ctx->log_fp);
        ctx->log_fp = NULL;
    }
}

static inline void pass(jfs_test_ctx *ctx, const char *desc)
{
    ctx->pass_count++;
    printf("  PASS  %s\n", desc);
    if (ctx->log_fp) {
        fprintf(ctx->log_fp, "  PASS  %s\n", desc);
        fflush(ctx->log_fp);
    }
}

static inline void fail(jfs_test_ctx *ctx, const char *desc, const char *expected, const char *actual)
{
    ctx->fail_count++;
    printf("  FAIL  %s\n", desc);
    printf("        expected : %s\n", expected ? expected : "");
    printf("        actual   : %s\n", actual ? actual : "");
    if (ctx->log_fp) {
        fprintf(ctx->log_fp, "  FAIL  %s\n", desc);
        fprintf(ctx->log_fp, "        expected : %s\n", expected ? expected : "");
        fprintf(ctx->log_fp, "        actual   : %s\n", actual ? actual : "");
        fflush(ctx->log_fp);
    }
}

static inline void assert_eq(jfs_test_ctx *ctx, const char *desc, const char *expected, const char *actual)
{
    if (!expected && !actual) {
        pass(ctx, desc);
        return;
    }
    if (expected && actual && strcmp(expected, actual) == 0) {
        pass(ctx, desc);
    } else {
        fail(ctx, desc, expected, actual);
    }
}

static inline void assert_ne(jfs_test_ctx *ctx, const char *desc, const char *not_expected, const char *actual)
{
    if (!actual || strcmp(not_expected, actual) != 0) {
        pass(ctx, desc);
    } else {
        char exp[256];
        snprintf(exp, sizeof(exp), "different from '%s'", not_expected);
        fail(ctx, desc, exp, actual);
    }
}

static inline void assert_has(jfs_test_ctx *ctx, const char *desc, const char *needle, const char *haystack)
{
    if (haystack && needle && strstr(haystack, needle) != NULL) {
        pass(ctx, desc);
    } else {
        char exp[256];
        snprintf(exp, sizeof(exp), "output containing '%s'", needle ? needle : "");
        fail(ctx, desc, exp, haystack);
    }
}

static inline void assert_not_has(jfs_test_ctx *ctx, const char *desc, const char *needle, const char *haystack)
{
    if (!haystack || !needle || strstr(haystack, needle) == NULL) {
        pass(ctx, desc);
    } else {
        char exp[256];
        snprintf(exp, sizeof(exp), "output without '%s'", needle);
        fail(ctx, desc, exp, haystack);
    }
}

static inline void assert_status(jfs_test_ctx *ctx, const char *desc, int expected, int actual)
{
    if (expected == actual) {
        pass(ctx, desc);
    } else {
        char exp[64], act[64];
        snprintf(exp, sizeof(exp), "exit status %d", expected);
        snprintf(act, sizeof(act), "%d", actual);
        fail(ctx, desc, exp, act);
    }
}

/* Execute command via fork/pipe/execvp, capturing stdout & stderr */
static inline jfs_cmd_res jfs_exec(char *const argv[])
{
    jfs_cmd_res res = {0};
    int pipefd[2];

    if (pipe(pipefd) != 0) {
        res.status = -1;
        return res;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        res.status = -1;
        return res;
    }

    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(argv[0], argv);
        _exit(127);
    }

    /* Parent */
    close(pipefd[1]);
    size_t cap = 4096;
    res.output = (char *)malloc(cap);
    res.length = 0;

    char buf[1024];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
        if (res.length + (size_t)n + 1 >= cap) {
            cap = (cap * 2) + (size_t)n + 1;
            res.output = (char *)realloc(res.output, cap);
        }
        memcpy(res.output + res.length, buf, (size_t)n);
        res.length += (size_t)n;
    }
    close(pipefd[0]);
    if (res.output)
        res.output[res.length] = '\0';

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        res.status = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        res.status = -WTERMSIG(status);
    else
        res.status = status;

    return res;
}

/* Build argv array from NULL-terminated list of strings */
static inline jfs_cmd_res jfs_run_args_impl(const char *img, ...)
{
    va_list ap;
    va_start(ap, img);
    size_t count = 0;
    const char *arg;
    while ((arg = va_arg(ap, const char *)) != NULL) {
        count++;
    }
    va_end(ap);

    const char *bin = jfs_get_bin();
    size_t total = 1 + (img ? 2 : 0) + count + 1;
    char **argv = (char **)calloc(total, sizeof(char *));
    size_t idx = 0;
    argv[idx++] = (char *)bin;
    if (img) {
        argv[idx++] = (char *)"-i";
        argv[idx++] = (char *)img;
    }
    va_start(ap, img);
    while ((arg = va_arg(ap, const char *)) != NULL) {
        argv[idx++] = (char *)arg;
    }
    va_end(ap);
    argv[idx] = NULL;

    jfs_cmd_res res = jfs_exec(argv);
    free(argv);
    return res;
}

#define JFS_RUN(img, ...) jfs_run_args_impl(img, __VA_ARGS__, NULL)

/* Build argv array and call jfs_exec (extra_args is NULL-terminated or length n_extra) */
static inline jfs_cmd_res jfs_run_v(const char *img, const char *const extra_args[], size_t n_extra)
{
    const char *bin = jfs_get_bin();
    size_t count = 0;
    if (extra_args) {
        if (n_extra > 0) {
            count = n_extra;
        } else {
            while (extra_args[count] != NULL) count++;
        }
    }
    size_t total = 1 + (img ? 2 : 0) + count + 1;
    char **argv = (char **)calloc(total, sizeof(char *));
    size_t idx = 0;
    argv[idx++] = (char *)bin;
    if (img) {
        argv[idx++] = (char *)"-i";
        argv[idx++] = (char *)img;
    }
    for (size_t i = 0; i < count; i++) {
        argv[idx++] = (char *)extra_args[i];
    }
    argv[idx] = NULL;

    jfs_cmd_res res = jfs_exec(argv);
    free(argv);
    return res;
}

#define JFS_RUN_ARR(img, arr) jfs_run_v(img, arr, sizeof(arr)/sizeof((arr)[0]))

/* Assert filesystem consistency by running check */
static inline void assert_consistent(jfs_test_ctx *ctx, const char *desc, const char *img)
{
    const char *args[] = {"--quiet", "-c", "mount", "-c", "check"};
    jfs_cmd_res res = JFS_RUN_ARR(img, args);
    assert_has(ctx, desc, "is consistent", res.output);
    jfs_cmd_res_free(&res);
}

static inline int jfs_test_summary(jfs_test_ctx *ctx)
{
    printf("\n%s: %d passed, %d failed\n", ctx->name, ctx->pass_count, ctx->fail_count);
    if (ctx->log_fp) {
        fprintf(ctx->log_fp, "\n%s: %d passed, %d failed\n", ctx->name, ctx->pass_count, ctx->fail_count);
        fprintf(ctx->log_fp, "log written to %s\n", ctx->logfile);
        fflush(ctx->log_fp);
    }
    printf("log written to %s\n", ctx->logfile);
    int rc = (ctx->fail_count != 0) ? 1 : 0;
    jfs_test_cleanup(ctx);
    return rc;
}

#endif /* JFS_TEST_FRAMEWORK_H */
