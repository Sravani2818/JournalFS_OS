/*
 * fsstress.c -- randomized workload driver for JournalFS.
 *
 * This is a *test tool*, not part of the filesystem.  It links against the
 * same objects as the CLI (everything except main.o) and drives the public
 * API directly, so a stress run performs one process-local transaction per
 * operation instead of spawning a process per command.
 *
 * What it checks
 * --------------
 * A shadow model of every file it touches is kept in host memory.  After each
 * operation the file is read back through the filesystem and compared with the
 * model, so a defect is caught at the operation that caused it.
 *
 * At the end (and after every remount) the whole tree is verified again, the
 * consistency checker is run, and -- unless --keep is given -- the image is
 * remounted one last time to prove the data survived a clean shutdown.
 *
 * Crash mode
 * ----------
 * `--crash-after N --crash-point P` arms the engine's deterministic crash
 * injection so the Nth operation aborts the process with exit status 99
 * exactly like a power loss.  `--adopt` then mounts whatever survived the
 * crash, reloads it into the model and continues with fresh random operations:
 * nothing may be torn, and every later operation must still be correct.
 *
 * Usage:  fsstress [options]
 *   --image PATH      image file (default build/stress.img)
 *   --size MB         image size in MiB when formatting (default 4)
 *   --seed N          PRNG seed (default 1)
 *   --ops N           number of operations (default 300)
 *   --crash-after N   abort the process during operation N (0 = never)
 *   --crash-point P   crash point name (see crash.h)
 *   --adopt           mount an existing image and model its current contents
 *   --no-journal      baseline mode: no write ahead log, no fsync barriers
 *                     (benchmarks only, see docs/journal_protocol.md)
 *   --format          force a fresh format before running
 *   --keep            do not verify after the final unmount
 *   --quiet           only print failures and the summary line
 *   --help
 *
 * Exit status: 0 = the whole run verified, 1 = verification failure,
 * 2 = usage error, 99 = injected crash (deliberate).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "journalfs.h"
#include "recovery.h"
#include "crash.h"
#include "util.h"
#include "inode.h"

#define MAX_FILES 32
#define MAX_NAME  48
#define MAX_DATA  (20u * 1024u)     /* per-file model cap, keeps runs quick */
#define MAX_ARGV  16

/* ------------------------------------------------------------------ */
/* tiny PRNG (xorshift64*), fixed so a seed reproduces a run exactly   */
/* ------------------------------------------------------------------ */

static uint64_t rng_state = 88172645463325252ull;

static void rng_seed(uint64_t s)
{
    if (s == 0)
        s = 88172645463325252ull;
    rng_state = s;
}

static uint64_t rng_next(void)
{
    uint64_t x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 2685821657736338717ull;
}

static uint32_t rng_range(uint32_t lo, uint32_t hi) /* inclusive */
{
    if (hi <= lo)
        return lo;
    return lo + (uint32_t)(rng_next() % (uint64_t)(hi - lo + 1));
}

/* ------------------------------------------------------------------ */
/* shadow model                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char     path[MAX_NAME];    /* "" = unused slot                        */
    uint8_t *data;
    size_t   cap;
    size_t   len;
    uint32_t mode;
} model_file;

static model_file model[MAX_FILES];
static unsigned   dirs_created;         /* /d0 .. are created on demand    */
static unsigned   ops_done;
static unsigned   ops_failed;           /* clean errors (ENOSPC etc.)      */
static bool       quiet;

static const char *path_of(unsigned i)
{
    static char buf[2][MAX_NAME];
    static int  which;

    which = !which;
    if (i < 20)
        snprintf(buf[which], MAX_NAME, "/s%u", i);
    else
        snprintf(buf[which], MAX_NAME, "/d%u/f%u", i - 20, i);
    return buf[which];
}

static model_file *model_slot(unsigned i)
{
    return &model[i];
}

static int model_find(const char *path)
{
    for (unsigned i = 0; i < MAX_FILES; i++)
        if (model[i].path[0] && strcmp(model[i].path, path) == 0)
            return (int)i;
    return -1;
}

static int model_alloc(const char *path)
{
    for (unsigned i = 0; i < MAX_FILES; i++) {
        if (model[i].path[0] == '\0') {
            jfs_strcpy(model[i].path, sizeof(model[i].path), path);
            model[i].len = 0;
            model[i].mode = 0644;
            return (int)i;
        }
    }
    return -1;
}

static void model_free(model_file *f)
{
    free(f->data);
    f->data = NULL;
    f->cap = f->len = 0;
    f->path[0] = '\0';
}

static int model_reserve(model_file *f, size_t want)
{
    if (want <= f->cap)
        return 0;
    size_t cap = f->cap ? f->cap : 64;
    while (cap < want)
        cap *= 2;
    uint8_t *p = realloc(f->data, cap);
    if (!p)
        return -1;
    f->data = p;
    f->cap = cap;
    return 0;
}

/* ------------------------------------------------------------------ */
/* verification helpers                                                */
/* ------------------------------------------------------------------ */

static unsigned verify_failures;

/*
 * "Clean" refusals: the filesystem is out of room and says so *before*
 * changing anything.  A stress run has to accept these -- a full image is a
 * legitimate state -- but it must then find the image exactly as it was.
 */
static bool clean_error(int rc)
{
    return rc == JFS_ERR_NOSPACE || rc == JFS_ERR_JFULL ||
           rc == JFS_ERR_NOINODES || rc == JFS_ERR_DIRFULL;
}

static void report(bool ok, const char *fmt, ...)
{
    va_list ap;
    if (ok)
        return;
    verify_failures++;
    va_start(ap, fmt);
    fputs("fsstress: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* Read a file back and compare it with the model byte for byte. */
static void verify_file(const model_file *f)
{
    jfs_buf buf;
    int rc;

    jfs_buf_init(&buf);
    rc = jfs_read(f->path, &buf);
    if (rc != JFS_OK) {
        report(false, "read %s failed: %s", f->path, jfs_strerror(rc));
        jfs_buf_free(&buf);
        return;
    }
    if ((size_t)buf.len != f->len ||
        (f->len > 0 && memcmp(buf.data, f->data, f->len) != 0)) {
        report(false, "%s: content differs (model %zu bytes, image %zu bytes)",
               f->path, f->len, (size_t)buf.len);
    }
    jfs_buf_free(&buf);
}

static void verify_all(const char *when)
{
    unsigned n = 0;

    for (unsigned i = 0; i < MAX_FILES; i++) {
        if (model[i].path[0] == '\0')
            continue;
        n++;
        verify_file(&model[i]);
    }

    char err[256] = "";
    int rc = jfs_check_consistency(jfs_fs_get(), err, sizeof(err));
    report(rc == JFS_OK, "%s: consistency check failed: %s", when, err);

    char cmd[160];
    snprintf(cmd, sizeof(cmd), "after %s: %u file(s) checked", when, n);
    if (!quiet && verify_failures == 0)
        printf("  ok   %s\n", cmd);
}

/* ------------------------------------------------------------------ */
/* operations                                                          */
/* ------------------------------------------------------------------ */

static unsigned live_files(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < MAX_FILES; i++)
        if (model[i].path[0])
            n++;
    return n;
}

static int pick_live(void)
{
    unsigned n = live_files();
    if (n == 0)
        return -1;
    unsigned pick = rng_range(0, n - 1);
    unsigned seen = 0;
    for (unsigned i = 0; i < MAX_FILES; i++) {
        if (!model[i].path[0])
            continue;
        if (seen++ == pick)
            return (int)i;
    }
    return -1;
}

static void op_create(void)
{
    if (live_files() >= MAX_FILES)
        return;
    unsigned i = rng_range(0, MAX_FILES - 1);
    const char *path = path_of(i);
    if (model_find(path) >= 0)
        return;

    /* files /dN/fM live in a subdirectory, which exercises path walking and
     * recursive removal; create the directories up to the one needed */
    while (i >= 20 && (unsigned)(i - 20) >= dirs_created) {
        char dir[MAX_NAME];
        snprintf(dir, sizeof(dir), "/d%u", dirs_created);
        int rc = jfs_mkdir(dir, 0755);
        if (rc != JFS_OK && rc != JFS_ERR_EXISTS) {
            if (clean_error(rc))
                ops_failed++;
            else
                report(false, "mkdir %s failed: %s", dir, jfs_strerror(rc));
            return;
        }
        dirs_created++;
    }
    {
        int rc = jfs_create(path, 0644);
        if (clean_error(rc)) {
            /* refused: the name must not exist afterwards */
            if (jfs_exists(path) != JFS_ERR_NOTFOUND)
                report(false, "%s exists after a refused create", path);
            ops_failed++;
            return;
        }
        if (rc != JFS_OK) {
            report(false, "create %s failed: %s", path, jfs_strerror(rc));
            return;
        }
    }
    int s = model_alloc(path);
    if (s < 0)
        return;
    model[s].len = 0;
    verify_file(&model[s]);
}

static void op_write(model_file *f, bool append)
{
    size_t len = rng_range(1, 700);
    uint64_t off;

    if (append) {
        off = f->len;
    } else {
        if (f->len == 0)
            off = 0;
        else
            off = rng_range(0, (uint32_t)(f->len - 1));
    }
    if (off + len > MAX_DATA)
        return;

    uint8_t tmp[700];
    for (size_t i = 0; i < len; i++)
        tmp[i] = (uint8_t)rng_range('a', 'z');

    int rc = jfs_write(f->path, tmp, len, off, append, false);
    if (rc != JFS_OK) {
        /* running out of room is a legitimate answer, and the model must not
         * change in that case; anything else is a bug. */
        if (clean_error(rc)) {
            ops_failed++;
            verify_file(f);
            return;
        }
        report(false, "write %s off=%llu len=%zu failed: %s", f->path,
               (unsigned long long)off, len, jfs_strerror(rc));
        return;
    }

    if (model_reserve(f, (size_t)off + len) != 0) {
        report(false, "out of host memory in the model");
        exit(1);
    }
    memcpy(f->data + off, tmp, len);
    if ((size_t)off + len > f->len)
        f->len = (size_t)off + len;
    verify_file(f);
}

static void op_truncate(model_file *f)
{
    /*
     * Half of the truncates pick an arbitrary size in [0, MAX_DATA], which
     * both grows and shrinks files by large amounts; the other half shrinks
     * by a small amount.  Growing is the interesting direction: the new data
     * has to be recorded, and growing inside the block that already holds the
     * end of the file is what the record reuse logic has to get right.
     */
    uint32_t n;
    if (f->len == 0 || rng_range(1, 100) <= 50)
        n = rng_range(0, MAX_DATA);
    else
        n = rng_range(0, (uint32_t)f->len);

    int rc = jfs_truncate(f->path, n);
    if (rc != JFS_OK) {
        if (clean_error(rc)) {
            ops_failed++;
            verify_file(f);
            return;
        }
        report(false, "truncate %s failed: %s", f->path, jfs_strerror(rc));
        return;
    }
    if (model_reserve(f, n) != 0) {
        report(false, "out of host memory in the model");
        exit(1);
    }
    if (n > f->len)
        memset(f->data + f->len, 0, n - f->len);
    f->len = n;
    verify_file(f);
}

static void op_remove(model_file *f)
{
    int rc = jfs_rm(f->path, false);

    if (rc != JFS_OK) {
        if (clean_error(rc)) {
            ops_failed++;
            if (jfs_exists(f->path) != JFS_OK)
                report(false, "%s vanished after a refused rm", f->path);
            else
                verify_file(f);
            return;
        }
        report(false, "rm %s failed: %s", f->path, jfs_strerror(rc));
        return;
    }
    if (jfs_exists(f->path) != JFS_ERR_NOTFOUND)
        report(false, "%s still exists after rm", f->path);
    model_free(f);
}

/* "/dN/..." -> N, or -1 when the path is not inside a numbered directory */
static int dir_index_of(const char *path)
{
    if (path[0] != '/' || path[1] != 'd')
        return -1;
    return (int)strtoul(path + 2, NULL, 10);
}

static void op_rename(model_file *f)
{
    unsigned i = rng_range(0, MAX_FILES - 1);
    const char *to = path_of(i);
    int dir = dir_index_of(to);

    if (strcmp(to, f->path) == 0 || model_find(to) >= 0)
        return;
    if (dir >= 0 && (unsigned)dir >= dirs_created)
        return;                              /* the directory does not exist */

    /* copy the target path before renaming: path_of() reuses its buffer */
    char old[MAX_NAME];
    jfs_strcpy(old, sizeof(old), f->path);

    int rc = jfs_rename(old, to);
    if (rc != JFS_OK) {
        if (clean_error(rc)) {
            ops_failed++;
            /* a refused rename must leave both names as they were */
            if (jfs_exists(old) != JFS_OK)
                report(false, "%s vanished during a refused rename", old);
            if (jfs_exists(to) != JFS_ERR_NOTFOUND)
                report(false, "%s appeared during a refused rename", to);
            return;
        }
        report(rc == JFS_ERR_EXISTS || rc == JFS_ERR_ISDIR,
               "rename %s -> %s failed: %s", old, to, jfs_strerror(rc));
        return;
    }
    jfs_strcpy(f->path, sizeof(f->path), to);
    verify_file(f);
}

static void op_sync(void)
{
    int rc = jfs_sync();
    report(rc == JFS_OK, "sync failed: %s", jfs_strerror(rc));
}

static void run_ops(unsigned count, unsigned crash_after, jfs_crash_point point)
{
    for (unsigned i = 0; i < count; i++) {
        ops_done++;
        if (crash_after && ops_done == crash_after) {
            /* arm the engine: the next matching stage aborts the process */
            jfs_crash_set(point);
        }

        unsigned roll = rng_range(1, 100);
        int pick;

        if (roll <= 25) {
            op_create();
            continue;
        }
        pick = pick_live();
        if (pick < 0)
            continue;
        model_file *f = model_slot((unsigned)pick);

        if (roll <= 50)
            op_write(f, false);
        else if (roll <= 70)
            op_write(f, true);
        else if (roll <= 80)
            op_truncate(f);
        else if (roll <= 88)
            op_rename(f);
        else if (roll <= 94)
            verify_file(f);
        else if (roll <= 98)
            op_remove(f);
        else
            op_sync();

        if (verify_failures)
            return;                          /* stop at the first defect */
    }
}

/* ------------------------------------------------------------------ */
/* image handling                                                      */
/* ------------------------------------------------------------------ */

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int do_format(const char *image, uint64_t size_bytes, bool force)
{
    jfs_format_opts o;
    char err[256] = "";

    memset(&o, 0, sizeof(o));
    o.size_bytes = size_bytes;
    o.force = force;

    int rc = jfs_format(image, &o, err, sizeof(err));
    if (rc != JFS_OK) {
        fprintf(stderr, "fsstress: format %s failed: %s\n", image,
                err[0] ? err : jfs_strerror(rc));
        return -1;
    }
    return 0;
}

static int do_mount(const char *image)
{
    char err[256] = "";

    int rc = jfs_mount(image, true, err, sizeof(err));
    if (rc != JFS_OK) {
        fprintf(stderr, "fsstress: mount %s failed: %s\n", image,
                err[0] ? err : jfs_strerror(rc));
        return -1;
    }
    return 0;
}

/*
 * Build the model from what is actually on the image.  Used after a crash:
 * the recovered state is the truth, and new random operations are applied on
 * top of it, so a lost (never committed) operation is not a failure.
 */
static int adopt_contents(void)
{
    for (unsigned i = 0; i < MAX_FILES; i++) {
        const char *path = path_of(i);
        jfs_buf buf;

        jfs_buf_init(&buf);
        int rc = jfs_read(path, &buf);
        if (rc != JFS_OK) {
            jfs_buf_free(&buf);
            continue;
        }
        int slot = model_find(path);
        if (slot < 0)
            slot = model_alloc(path);
        if (slot < 0) {
            jfs_buf_free(&buf);
            return -1;
        }
        model_file *f = model_slot((unsigned)slot);
        if (model_reserve(f, (size_t)buf.len) != 0) {
            jfs_buf_free(&buf);
            return -1;
        }
        memcpy(f->data, buf.data, (size_t)buf.len);
        f->len = (size_t)buf.len;
        jfs_buf_free(&buf);
    }
    /* remember how many subdirectories already exist */
    dirs_created = 0;
    for (unsigned d = 0; d < MAX_FILES; d++) {
        char dir[MAX_NAME];
        snprintf(dir, sizeof(dir), "/d%u", d);
        jfs_inode ino;
        if (jfs_lookup(dir, NULL, &ino) != JFS_OK)
            break;
        dirs_created++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    fputs("usage: fsstress [--image PATH] [--size MB] [--seed N] [--ops N]\n"
          "                [--crash-after N] [--crash-point NAME] [--adopt]\n"
          "                [--format] [--keep] [--quiet] [--help]\n", stderr);
}

int main(int argc, char **argv)
{
    const char *image = "build/stress.img";
    uint64_t size_mb = 4;
    uint64_t seed = 1;
    unsigned ops = 300;
    unsigned crash_after = 0;
    jfs_crash_point point = JFS_CRASH_NONE;
    bool adopt = false, force_format = false, keep = false;
    bool no_journal = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--image") && val)       image = argv[++i];
        else if (!strcmp(a, "--size") && val)   size_mb = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--seed") && val)   seed = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(a, "--ops") && val)    ops = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--crash-after") && val) crash_after = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--crash-point") && val) {
            point = jfs_crash_parse(argv[++i]);
            if (point >= JFS_CRASH__COUNT) {
                fprintf(stderr, "fsstress: unknown crash point\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--adopt"))  adopt = true;
        else if (!strcmp(a, "--format")) force_format = true;
        else if (!strcmp(a, "--keep"))   keep = true;
        else if (!strcmp(a, "--no-journal")) no_journal = true;
        else if (!strcmp(a, "--quiet"))  quiet = true;
        else if (!strcmp(a, "--help")) { usage(); return 0; }
        else { usage(); return 2; }
    }
    if (crash_after && point == JFS_CRASH_NONE)
        point = JFS_CRASH_POST_COMMIT_SYNC;

    rng_seed(seed);

    if (force_format || !file_exists(image)) {
        if (do_format(image, size_mb * 1024 * 1024, true) != 0)
            return 1;
    }
    if (do_mount(image) != 0)
        return 1;

    if (no_journal) {
        /* Baseline comparison mode: same driver, same storage, but the
         * write ahead log and its fsync barriers are switched off. */
        jfs_fs_get()->journal_enabled = false;
        if (!quiet)
            printf("fsstress: write ahead logging is OFF (baseline mode)\n");
    }

    if (adopt && adopt_contents() != 0) {
        fprintf(stderr, "fsstress: cannot adopt the recovered contents\n");
        return 1;
    }

    if (!quiet)
        printf("fsstress: seed=%llu ops=%u image=%s%s\n", (unsigned long long)seed,
               ops, image, adopt ? " (adopting existing contents)" : "");

    run_ops(ops, crash_after, point);

    if (verify_failures) {
        fprintf(stderr, "fsstress: %u verification failure(s) after %u operation(s)\n",
                verify_failures, ops_done);
        jfs_unmount();
        return 1;
    }

    verify_all("final pass");
    if (jfs_unmount() != JFS_OK) {
        fprintf(stderr, "fsstress: unmount failed\n");
        return 1;
    }

    if (!keep) {
        /* the real proof of durability: mount a fresh process view of the
         * image and compare everything again */
        if (do_mount(image) != 0)
            return 1;
        verify_all("remount");
        if (jfs_unmount() != JFS_OK) {
            fprintf(stderr, "fsstress: unmount failed\n");
            return 1;
        }
    }

    if (verify_failures) {
        fprintf(stderr, "fsstress: %u verification failure(s)\n", verify_failures);
        return 1;
    }
    /* The summary is always printed: a caller that asked for `--quiet` still
     * wants to know how much work was done and how often the filesystem
     * refused it (that is not a failure). */
    printf("fsstress: OK -- %u operation(s), %u clean error(s), %u file(s) verified\n",
           ops_done, ops_failed, live_files());
    return 0;
}
