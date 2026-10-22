/*
 * benchmark.c -- C11 implementation of the JournalFS benchmark runner.
 *
 * Measures performance of JournalFS (journaled mode vs. baseline --no-journal)
 * and crash recovery replay times. Generates results/benchmark.csv and
 * results/benchmark.md without relying on bash or python scripts.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "test_framework.h"

#define DEFAULT_RUNS 3
#define IMAGE_MB 8

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int cmp_uint64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static uint64_t median(uint64_t *arr, size_t n)
{
    if (n == 0) return 0;
    qsort(arr, n, sizeof(uint64_t), cmp_uint64);
    return arr[n / 2];
}

static void build_image(const char *img)
{
    unlink(img);
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "format %s --size %um", img, IMAGE_MB);
    const char *args[] = {"-q", "-c", fmt};
    jfs_cmd_res res = JFS_RUN_ARR(NULL, args);
    jfs_cmd_res_free(&res);
}

static uint64_t time_script(const char *img, const char *script, bool no_journal, unsigned group_commit)
{
    uint64_t t0 = now_ms();
    if (no_journal) {
        const char *args[] = {"--quiet", "--no-journal", "-f", script};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    } else if (group_commit > 1) {
        char gc_buf[16];
        snprintf(gc_buf, sizeof(gc_buf), "%u", group_commit);
        const char *args[] = {"--quiet", "--group-commit", gc_buf, "-f", script};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    } else {
        const char *args[] = {"--quiet", "-f", script};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    uint64_t t1 = now_ms();
    uint64_t diff = (t1 >= t0) ? (t1 - t0) : 1;
    return (diff == 0) ? 1 : diff;
}

typedef struct {
    char label[64];
    char mode[24];
    uint64_t ms;
    uint64_t ops_per_s;
    double mb_per_s;
    uint64_t bytes;
} bench_row;

static bench_row g_rows[48];
static size_t g_nrows = 0;

typedef void (*workload_gen_fn)(const char *script_path);

static void gen_create(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "mount\nmkdir /d\n");
    for (int i = 1; i <= 240; i++) {
        fprintf(fp, "create /d/f%d\nwrite /d/f%d hello-%d\nappend /d/f%d more\nappend /d/f%d text\n", i, i, i, i, i);
    }
    fprintf(fp, "sync\nunmount\n");
    fclose(fp);
}

static void gen_append(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "mount\ncreate /log\n");
    for (int i = 1; i <= 3000; i++) {
        fprintf(fp, "append /log line-%d-payload\n", i);
    }
    fprintf(fp, "sync\nunmount\n");
    fclose(fp);
}

static void gen_rewrite(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "mount\ncreate /f\nfill /f 8192 A\n");
    for (int i = 1; i <= 600; i++) {
        fprintf(fp, "fill /f 8192 B\n");
    }
    fprintf(fp, "sync\nunmount\n");
    fclose(fp);
}

static void gen_bulk(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "mount\n");
    for (int i = 1; i <= 20; i++) {
        fprintf(fp, "create /big%d\nfill /big%d 64000 x\n", i, i);
    }
    fprintf(fp, "sync\nunmount\n");
    fclose(fp);
}

static void gen_churn(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "mount\nmkdir /c\n");
    for (int i = 1; i <= 600; i++) {
        fprintf(fp, "create /c/a%d\nrename /c/a%d /c/b%d\nrm /c/b%d\n", i, i, i, i);
    }
    fprintf(fp, "sync\nunmount\n");
    fclose(fp);
}

static void gen_mkdir_rm(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "mount\nmkdir /base\n");
    for (int i = 1; i <= 1500; i++) {
        fprintf(fp, "mkdir /base/d%d\nrmdir /base/d%d\n", i, i);
    }
    fprintf(fp, "sync\nunmount\n");
    fclose(fp);
}

static void gen_read(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "mount\ncreate /r\nfill /r 32768 z\n");
    for (int i = 1; i <= 3000; i++) {
        fprintf(fp, "cat /r\n");
    }
    fprintf(fp, "unmount\n");
    fclose(fp);
}

static void run_case(const char *workdir, const char *label, uint64_t ops, uint64_t bytes,
                     workload_gen_fn gen_fn, int runs)
{
    char img[512], script[512];
    snprintf(img, sizeof(img), "%s/bench.img", workdir);
    snprintf(script, sizeof(script), "%s/w.jfs", workdir);

    gen_fn(script);

    const char *modes[] = {"journaled", "baseline"};
    for (int m = 0; m < 2; m++) {
        bool no_j = (m == 1);
        uint64_t times[16];
        for (int r = 0; r < runs; r++) {
            build_image(img);
            times[r] = time_script(img, script, no_j, 1);
        }
        uint64_t med_ms = median(times, runs);
        if (med_ms == 0) med_ms = 1;

        uint64_t ops_per_s = (ops * 1000ull) / med_ms;
        double mb_per_s = ((double)bytes / 1048576.0) * 1000.0 / (double)med_ms;

        printf("  %-24s %-9s %5llu ms   %8llu ops/s   %8.2f MB/s\n",
               label, modes[m], (unsigned long long)med_ms,
               (unsigned long long)ops_per_s, mb_per_s);

        bench_row *row = &g_rows[g_nrows++];
        snprintf(row->label, sizeof(row->label), "%s", label);
        snprintf(row->mode, sizeof(row->mode), "%s", modes[m]);
        row->ms = med_ms;
        row->ops_per_s = ops_per_s;
        row->mb_per_s = mb_per_s;
        row->bytes = bytes;
    }
}

static void run_group_commit_case(const char *workdir, int runs)
{
    char img[512], script[512];
    snprintf(img, sizeof(img), "%s/bench_gc.img", workdir);
    snprintf(script, sizeof(script), "%s/w_gc.jfs", workdir);

    gen_append(script);
    uint64_t ops = 3000;
    uint64_t bytes = 60000;

    unsigned batches[] = {1, 4, 8};
    const char *names[] = {"per-tx (batch=1)", "group-commit (batch=4)", "group-commit (batch=8)"};

    printf("\n--- Group Commit Throughput (append x3000) ---\n");
    for (int b = 0; b < 3; b++) {
        uint64_t times[16];
        for (int r = 0; r < runs; r++) {
            build_image(img);
            times[r] = time_script(img, script, false, batches[b]);
        }
        uint64_t med_ms = median(times, runs);
        if (med_ms == 0) med_ms = 1;

        uint64_t ops_per_s = (ops * 1000ull) / med_ms;
        double mb_per_s = ((double)bytes / 1048576.0) * 1000.0 / (double)med_ms;

        printf("  %-24s %-24s %5llu ms   %8llu ops/s   %8.2f MB/s\n",
               "group commit", names[b], (unsigned long long)med_ms,
               (unsigned long long)ops_per_s, mb_per_s);

        bench_row *row = &g_rows[g_nrows++];
        snprintf(row->label, sizeof(row->label), "append x3000 (gc=%u)", batches[b]);
        snprintf(row->mode, sizeof(row->mode), "%s", batches[b] == 1 ? "per-tx" : "group-commit");
        row->ms = med_ms;
        row->ops_per_s = ops_per_s;
        row->mb_per_s = mb_per_s;
        row->bytes = bytes;
    }
}

typedef struct {
    char case_name[64];
    uint64_t txns;
    uint64_t blocks;
    uint64_t payload_bytes;
    uint64_t recovery_ms;
} rec_row;

static rec_row g_rec_rows[8];
static size_t g_nrec_rows = 0;

static void measure_mount(const char *workdir, const char *src_img, const char *label,
                          uint64_t txns, uint64_t blocks, uint64_t bytes)
{
    char copy_img[512];
    snprintf(copy_img, sizeof(copy_img), "%s/rec_copy.img", workdir);
    uint64_t times[5];

    for (int i = 0; i < 5; i++) {
        /* Copy file */
        int sfd = open(src_img, O_RDONLY);
        int dfd = open(copy_img, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        char buf[8192];
        ssize_t n;
        while ((n = read(sfd, buf, sizeof(buf))) > 0) {
            if (write(dfd, buf, (size_t)n) != n) break;
        }
        close(sfd);
        close(dfd);

        uint64_t t0 = now_ms();
        const char *args[] = {"--quiet", "-c", "mount", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(copy_img, args);
        jfs_cmd_res_free(&res);
        uint64_t t1 = now_ms();
        times[i] = (t1 >= t0) ? (t1 - t0) : 1;
    }
    uint64_t med_ms = median(times, 5);
    printf("  %-34s %3llu txn(s), %3llu block(s), %8llu B  ->  %4llu ms\n",
           label, (unsigned long long)txns, (unsigned long long)blocks,
           (unsigned long long)bytes, (unsigned long long)med_ms);

    rec_row *r = &g_rec_rows[g_nrec_rows++];
    snprintf(r->case_name, sizeof(r->case_name), "%s", label);
    r->txns = txns;
    r->blocks = blocks;
    r->payload_bytes = bytes;
    r->recovery_ms = med_ms;
}

int main(int argc, char **argv)
{
    int runs = DEFAULT_RUNS;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
            runs = atoi(argv[++i]);
            if (runs <= 0) runs = 1;
        }
    }

    const char *outdir = getenv("JFS_RESULTS");
    if (!outdir || !*outdir) {
        if (access("results", F_OK) == 0) outdir = "results";
        else if (access("../results", F_OK) == 0) outdir = "../results";
        else { mkdir("results", 0755); outdir = "results"; }
    } else {
        mkdir(outdir, 0755);
    }

    char tmpl[] = "/tmp/jfs_bench_XXXXXX";
    char *tmp = mkdtemp(tmpl);
    const char *workdir = tmp ? tmp : "/tmp";

    printf("JournalFS benchmark (C11)\n");
    printf("image: %d MiB formatted before each timed run, median of %d run(s)\n\n", IMAGE_MB, runs);
    printf("--- workloads (each run: format, mount, work, sync, unmount) ---\n");

    run_case(workdir, "create 240 files", 960, 16000, gen_create, runs);
    run_case(workdir, "append x3000", 3000, 60000, gen_append, runs);
    run_case(workdir, "rewrite 8 KiB", 601, 4915200, gen_rewrite, runs);
    run_case(workdir, "bulk 62.5 KiB", 40, 1280000, gen_bulk, runs);
    run_case(workdir, "churn x600", 1800, 0, gen_churn, runs);
    run_case(workdir, "mkdir x1500", 3000, 0, gen_mkdir_rm, runs);
    run_case(workdir, "read 32 KiB", 3000, 98304000, gen_read, runs);
    run_group_commit_case(workdir, runs);

    printf("\n--- crash recovery (mount + replay, median of 5 copies of a crashed image) ---\n");
    char img[512];
    snprintf(img, sizeof(img), "%s/rec_clean.img", workdir);
    build_image(img);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "unmount"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    measure_mount(workdir, img, "clean mount (nothing to replay)", 0, 0, 0);

    /* 16 small appends */
    snprintf(img, sizeof(img), "%s/rec_small.img", workdir);
    build_image(img);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /log"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    char crash_sc[512];
    snprintf(crash_sc, sizeof(crash_sc), "%s/rec_small.jfs", workdir);
    FILE *fp = fopen(crash_sc, "w");
    if (fp) {
        fprintf(fp, "mount\n");
        for (int i = 0; i <= 16; i++) fprintf(fp, "append /log payload\n");
        fclose(fp);
    }
    {
        const char *args[] = {
            "--quiet", "-f", crash_sc, "--checkpoint-interval", "4096",
            "--crash-after", "16", "--crash", "post_commit_sync"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    measure_mount(workdir, img, "16 small appends pending", 16, 17, 16 * 7);

    /* 5 rewrites of 8 KiB */
    snprintf(img, sizeof(img), "%s/rec_big.img", workdir);
    build_image(img);
    {
        const char *args[] = {"--quiet", "-c", "mount", "-c", "create /big", "-c", "fill /big 8192 A"};
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    snprintf(crash_sc, sizeof(crash_sc), "%s/rec_big.jfs", workdir);
    fp = fopen(crash_sc, "w");
    if (fp) {
        fprintf(fp, "mount\n");
        for (int i = 0; i <= 6; i++) fprintf(fp, "fill /big 8192 B\n");
        fclose(fp);
    }
    {
        const char *args[] = {
            "--quiet", "-f", crash_sc, "--checkpoint-interval", "4096",
            "--crash-after", "5", "--crash", "post_commit_sync"
        };
        jfs_cmd_res res = JFS_RUN_ARR(img, args);
        jfs_cmd_res_free(&res);
    }
    measure_mount(workdir, img, "5 x 8 KiB rewrites pending", 5, 15, 5 * 8192);

    /* Write CSV and Markdown */
    char csv_path[512], md_path[512];
    snprintf(csv_path, sizeof(csv_path), "%s/benchmark.csv", outdir);
    snprintf(md_path, sizeof(md_path), "%s/benchmark.md", outdir);

    printf("\n--- writing %s and %s ---\n", csv_path, md_path);
    FILE *fcsv = fopen(csv_path, "w");
    if (fcsv) {
        fprintf(fcsv, "workload,mode,time_ms,ops_per_s,mb_per_s,bytes\n");
        for (size_t i = 0; i < g_nrows; i++) {
            fprintf(fcsv, "%s,%s,%llu,%llu,%.2f,%llu\n",
                    g_rows[i].label, g_rows[i].mode,
                    (unsigned long long)g_rows[i].ms,
                    (unsigned long long)g_rows[i].ops_per_s,
                    g_rows[i].mb_per_s,
                    (unsigned long long)g_rows[i].bytes);
        }
        fclose(fcsv);
    }

    FILE *fmd = fopen(md_path, "w");
    if (fmd) {
        fprintf(fmd, "# JournalFS benchmark\n\n");
        fprintf(fmd, "- image: %d MiB formatted before each timed run, median of %d run(s)\n", IMAGE_MB, runs);
        fprintf(fmd, "- binary: `build/journalfs` (baseline mode is `--no-journal`)\n\n");
        fprintf(fmd, "| workload | mode | time (ms) | ops/s | MB/s | payload bytes |\n");
        fprintf(fmd, "|---|---|---|---|---|---|\n");
        for (size_t i = 0; i < g_nrows; i++) {
            fprintf(fmd, "| %s | %s | %llu | %llu | %.2f | %llu |\n",
                    g_rows[i].label, g_rows[i].mode,
                    (unsigned long long)g_rows[i].ms,
                    (unsigned long long)g_rows[i].ops_per_s,
                    g_rows[i].mb_per_s,
                    (unsigned long long)g_rows[i].bytes);
        }

        fprintf(fmd, "\n## Crash recovery\n\n");
        fprintf(fmd, "| case | transactions pending | journal blocks used | redo payload bytes | mount + replay (ms) |\n");
        fprintf(fmd, "|---|---|---|---|---|\n");
        for (size_t i = 0; i < g_nrec_rows; i++) {
            fprintf(fmd, "| %s | %llu | %llu | %llu | %llu |\n",
                    g_rec_rows[i].case_name,
                    (unsigned long long)g_rec_rows[i].txns,
                    (unsigned long long)g_rec_rows[i].blocks,
                    (unsigned long long)g_rec_rows[i].payload_bytes,
                    (unsigned long long)g_rec_rows[i].recovery_ms);
        }

        fprintf(fmd, "\n## Methodology and Notes\n\n");
        fprintf(fmd, "* `ops/s` measures transactions committed and persisted per second.\n");
        fprintf(fmd, "* Journaling performs synchronous write-ahead logging before updates reach home location.\n");
        fprintf(fmd, "* Memory management uses the bounded block buffer cache with fixed frames and LRU eviction.\n");
        fclose(fmd);
    }

    if (strncmp(workdir, "/tmp/jfs_bench_", 15) == 0) {
        nftw(workdir, jfs_unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
    }

    printf("done: %s, %s\n", csv_path, md_path);
    return 0;
}
