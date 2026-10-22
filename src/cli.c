/*
 * cli.c -- Layer 1, the terminal interface.
 *
 * The same command language is used interactively and by the test scripts
 * (`journalfs -i image -c "mount" -c "create /a"` or `-f script.txt`), so a
 * test never exercises a different code path than a human does.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "bcache.h"
#include "checksum.h"
#include "cli.h"
#include "crash.h"
#include "directory.h"
#include "disk_image.h"
#include "inode.h"
#include "permissions.h"
#include "journal.h"
#include "journalfs.h"
#include "recovery.h"
#include "storage.h"
#include "util.h"

typedef int (*cmd_fn)(int argc, char **argv, FILE *out);

typedef struct {
    const char *name;
    const char *usage;
    const char *summary;
    cmd_fn      fn;
    const char *category;
} cmd_entry;

static struct {
    jfs_cli_opts opts;
    char  image[512];
    char  cwd[1024];
    bool  mounted;
    int   failures;
    bool  exit_requested;
    long  exit_code;
} g;

static int cmd_format(int argc, char **argv, FILE *out);
static int cmd_mount(int argc, char **argv, FILE *out);
static int cmd_unmount(int argc, char **argv, FILE *out);
static int cmd_create(int argc, char **argv, FILE *out);
static int cmd_nano(int argc, char **argv, FILE *out);
static int cmd_write(int argc, char **argv, FILE *out);
static int cmd_read(int argc, char **argv, FILE *out);
static int cmd_ls(int argc, char **argv, FILE *out);
static int cmd_stat(int argc, char **argv, FILE *out);
static int cmd_rm(int argc, char **argv, FILE *out);
static int cmd_mkdir(int argc, char **argv, FILE *out);
static int cmd_rmdir(int argc, char **argv, FILE *out);
static int cmd_rename(int argc, char **argv, FILE *out);
static int cmd_truncate(int argc, char **argv, FILE *out);
static int cmd_chmod(int argc, char **argv, FILE *out);
static int cmd_fill(int argc, char **argv, FILE *out);
static int cmd_hash(int argc, char **argv, FILE *out);
static int cmd_cd(int argc, char **argv, FILE *out);
static int cmd_pwd(int argc, char **argv, FILE *out);
static int cmd_df(int argc, char **argv, FILE *out);
static int cmd_stats(int argc, char **argv, FILE *out);
static int cmd_inspect(int argc, char **argv, FILE *out);
static int cmd_sync(int argc, char **argv, FILE *out);
static int cmd_journal(int argc, char **argv, FILE *out);
static int cmd_check(int argc, char **argv, FILE *out);
static int cmd_set(int argc, char **argv, FILE *out);
static int cmd_echo(int argc, char **argv, FILE *out);
static int cmd_clear(int argc, char **argv, FILE *out);
static int cmd_whoami(int argc, char **argv, FILE *out);
static int cmd_history(int argc, char **argv, FILE *out);
static int cmd_help(int argc, char **argv, FILE *out);
static int cmd_banner(int argc, char **argv, FILE *out);
static int cmd_exit(int argc, char **argv, FILE *out);

static const cmd_entry commands[] = {
    {"format",   "format [PATH] [--size SIZE] [--force]", "format volume image", cmd_format, "Volume & Storage Lifecycle"},
    {"mount",    "mount [PATH]",              "mount and recover image", cmd_mount, "Volume & Storage Lifecycle"},
    {"unmount",  "unmount",                   "checkpoint and close image", cmd_unmount, "Volume & Storage Lifecycle"},
    {"inspect",  "inspect PATH",              "inspect superblock & WAL", cmd_inspect, "Volume & Storage Lifecycle"},

    {"create",   "create FILE [MODE]",        "create regular file", cmd_create, "File I/O & Data Operations"},
    {"touch",    "touch FILE [MODE]",         "create regular file", cmd_create, "File I/O & Data Operations"},
    {"nano",     "nano FILE",                 "interactive text editor", cmd_nano, "File I/O & Data Operations"},
    {"edit",     "edit FILE",                 "interactive text editor", cmd_nano, "File I/O & Data Operations"},
    {"write",    "write FILE TEXT",           "atomic overwrite via WAL", cmd_write, "File I/O & Data Operations"},
    {"append",   "append FILE TEXT",          "durable append via WAL", cmd_write, "File I/O & Data Operations"},
    {"read",     "read FILE",                 "read file with header", cmd_read, "File I/O & Data Operations"},
    {"cat",      "cat FILE",                  "print file content", cmd_read, "File I/O & Data Operations"},
    {"truncate", "truncate FILE SIZE",        "resize file", cmd_truncate, "File I/O & Data Operations"},
    {"fill",     "fill FILE SIZE [CHAR]",     "fill repeated pattern", cmd_fill, "File I/O & Data Operations"},
    {"hash",     "hash FILE",                 "CRC-32C file checksum", cmd_hash, "File I/O & Data Operations"},

    {"mkdir",    "mkdir PATH [MODE]",         "create directory", cmd_mkdir, "Directory Hierarchy & Paths"},
    {"md",       "md PATH [MODE]",            "create directory", cmd_mkdir, "Directory Hierarchy & Paths"},
    {"rmdir",    "rmdir PATH",                "remove empty directory", cmd_rmdir, "Directory Hierarchy & Paths"},
    {"rd",       "rd PATH",                   "remove empty directory", cmd_rmdir, "Directory Hierarchy & Paths"},
    {"ls",       "ls [PATH]",                 "list directory contents", cmd_ls, "Directory Hierarchy & Paths"},
    {"cd",       "cd PATH",                   "change directory", cmd_cd, "Directory Hierarchy & Paths"},
    {"pwd",      "pwd",                       "print working directory", cmd_pwd, "Directory Hierarchy & Paths"},

    {"stat",     "stat FILE",                 "display inode metadata", cmd_stat, "Metadata & Access Permissions"},
    {"chmod",    "chmod MODE FILE",           "change permission bits", cmd_chmod, "Metadata & Access Permissions"},
    {"rename",   "rename OLD NEW",            "rename file or directory", cmd_rename, "Metadata & Access Permissions"},
    {"rm",       "rm [-r] FILE",              "remove file or tree", cmd_rm, "Metadata & Access Permissions"},
    {"del",      "del [-r] FILE",             "remove file or tree", cmd_rm, "Metadata & Access Permissions"},

    {"sync",     "sync",                      "flush dirty buffers & sync", cmd_sync, "Journaling, Consistency & Telemetry"},
    {"journal",  "journal",                   "inspect active WAL records", cmd_journal, "Journaling, Consistency & Telemetry"},
    {"check",    "check",                     "verify filesystem integrity", cmd_check, "Journaling, Consistency & Telemetry"},
    {"df",       "df",                        "display space usage", cmd_df, "Journaling, Consistency & Telemetry"},
    {"stats",    "stats",                     "display engine telemetry", cmd_stats, "Journaling, Consistency & Telemetry"},
    {"set",      "set checkpoint-interval N", "tune engine parameters", cmd_set, "Journaling, Consistency & Telemetry"},

    {"echo",     "echo [TEXT]",               "print text", cmd_echo, "Shell Utilities & Session"},
    {"clear",    "clear",                     "clear terminal screen", cmd_clear, "Shell Utilities & Session"},
    {"cls",      "cls",                       "clear terminal screen", cmd_clear, "Shell Utilities & Session"},
    {"whoami",   "whoami",                    "display current user", cmd_whoami, "Shell Utilities & Session"},
    {"history",  "history",                   "show command history", cmd_history, "Shell Utilities & Session"},
    {"banner",   "banner",                    "display header banner", cmd_banner, "Shell Utilities & Session"},
    {"logo",     "logo",                      "display header banner", cmd_banner, "Shell Utilities & Session"},
    {"help",     "help [COMMAND]",            "show command catalog", cmd_help, "Shell Utilities & Session"},
    {"exit",     "exit",                      "exit shell session", cmd_exit, "Shell Utilities & Session"},
};

#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

void jfs_cli_help(FILE *out)
{
    static const struct {
        const char *name;
        const char *color;
        const char *symbol;
        const char *dashes;
    } cats[] = {
        {"Volume & Storage Lifecycle",         JFS_COLOR_PURPLE,   "◆", "───────────────────────────────────────────────"},
        {"File I/O & Data Operations",         JFS_COLOR_SKY_BLUE, "◆", "───────────────────────────────────────────────"},
        {"Directory Hierarchy & Paths",        JFS_COLOR_GREEN,    "◆", "──────────────────────────────────────────────"},
        {"Metadata & Access Permissions",      JFS_COLOR_AMBER,    "◆", "────────────────────────────────────────────"},
        {"Journaling, Consistency & Telemetry", JFS_COLOR_PINK,    "◆", "──────────────────────────────────────"},
        {"Shell Utilities & Session",          JFS_COLOR_CYAN,     "◆", "────────────────────────────────────────────────"},
    };
    size_t c, i;
    bool col = jfs_color_enabled(out);

    if (col) {
        fprintf(out, "\n%sJournalFS Command Catalog%s %s(paths are absolute or relative to pwd)%s\n\n",
                JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_GRAY, JFS_COLOR_RESET);
    } else {
        fprintf(out, "JournalFS commands (paths are absolute or relative to pwd):\n\n");
    }

    for (c = 0; c < sizeof(cats) / sizeof(cats[0]); c++) {
        if (col) {
            fprintf(out, "  %s%s %s%s %s%s%s\n",
                    cats[c].color, cats[c].symbol,
                    cats[c].name, JFS_COLOR_RESET,
                    JFS_COLOR_GRAY, cats[c].dashes, JFS_COLOR_RESET);
        } else {
            fprintf(out, "=== %s ===\n", cats[c].name);
        }
        for (i = 0; i < NCOMMANDS; i++) {
            if (commands[i].category && strcmp(commands[i].category, cats[c].name) == 0) {
                if (col) {
                    fprintf(out, "    %s%-38s%s %s%s%s\n",
                            JFS_COLOR_WHITE, commands[i].usage, JFS_COLOR_RESET,
                            JFS_COLOR_LIGHT_GRAY, commands[i].summary, JFS_COLOR_RESET);
                } else {
                    fprintf(out, "  %-38s %s\n", commands[i].usage, commands[i].summary);
                }
            }
        }
        fputc('\n', out);
    }

    if (col) {
        fprintf(out, "  %sOptions:%s %s-i/--image PATH, -c/--command CMD, -f/--script FILE,\n"
                     "           --crash POINT, --crash-after N, --checkpoint-interval N, --group-commit N,\n"
                     "           --quiet, --verbose, --no-journal, --help, --version%s\n"
                     "  %sCrash Points:%s %ssee docs/journal_protocol.md; '--crash none' disables injection.%s\n\n",
                JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_LIGHT_GRAY, JFS_COLOR_RESET,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_LIGHT_GRAY, JFS_COLOR_RESET);
    } else {
        fprintf(out, "Options: -i/--image PATH, -c/--command CMD (repeatable), -f/--script FILE,\n"
                     "         --crash POINT, --crash-after N, --checkpoint-interval N, --group-commit N,\n"
                     "         --quiet, --verbose, --no-journal, --help, --version\n"
                     "Crash points: see docs/journal_protocol.md; '--crash none' disables injection.\n\n");
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void fail(FILE *out, const char *fmt, ...)
{
    va_list ap;
    bool col = jfs_color_enabled(out);

    if (col)
        fprintf(out, "%serror:%s ", JFS_COLOR_RED, JFS_COLOR_RESET);
    else
        fprintf(out, "error: ");
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    g.failures++;
}

static void emit_to_file(void *ud, const char *line)
{
    fprintf((FILE *)ud, "%s\n", line);
}

static int to_path(const char *arg, char *out, size_t outsz)
{
    if (!arg || jfs_path_normalize(g.cwd, arg, out, outsz) != JFS_OK) {
        g.failures++;
        return JFS_ERR_BADPATH;
    }
    return JFS_OK;
}

static int report(int rc, const char *what, FILE *out)
{
    if (rc == JFS_OK)
        return 0;
    fail(out, "%s: %s", what, jfs_strerror(rc));
    return 1;
}

/* Split a command line into arguments, honouring double quotes. */
static int tokenize(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max - 1) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"')
                p++;
            if (*p == '"')
                *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p))
                p++;
            if (*p)
                *p++ = '\0';
        }
    }
    argv[argc] = NULL;
    return argc;
}

/* Text argument handling for write/append: everything after the file name is
 * the text.  A fully quoted text gets \n, \t and \\ interpreted. */
static char *text_argument(int argc, char **argv, int first)
{
    size_t len = 0;
    char *buf;
    int i;
    bool quoted;

    if (argc <= first)
        return NULL;
    for (i = first; i < argc; i++)
        len += strlen(argv[i]) + 1;
    buf = (char *)malloc(len + 1);
    if (!buf)
        return NULL;
    buf[0] = '\0';
    for (i = first; i < argc; i++) {
        if (i > first)
            strcat(buf, " ");
        strcat(buf, argv[i]);
    }

    quoted = (first < argc && buf[0] == '"' && buf[strlen(buf) - 1] == '"' && strlen(buf) >= 2);
    if (quoted) {
        char *src = buf + 1;
        char *dst = buf;
        size_t n = strlen(src);
        if (n > 0)
            src[n - 1] = '\0';
        while (*src) {
            if (*src == '\\' && src[1] == 'n') {
                *dst++ = '\n';
                src += 2;
            } else if (*src == '\\' && src[1] == 't') {
                *dst++ = '\t';
                src += 2;
            } else if (*src == '\\' && src[1] == '\\') {
                *dst++ = '\\';
                src += 2;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* commands: image lifecycle                                           */
/* ------------------------------------------------------------------ */

static int cmd_format(int argc, char **argv, FILE *out)
{
    jfs_format_opts opts;
    const char *path = g.image;
    char err[256];
    int i, rc;

    memset(&opts, 0, sizeof(opts));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0) {
            opts.force = true;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            rc = jfs_parse_size(argv[++i], &opts.size_bytes);
            if (rc != JFS_OK) {
                fail(out, "format: bad size '%s'", argv[i]);
                return 1;
            }
        } else if (argv[i][0] == '-') {
            fail(out, "format: unknown option '%s'", argv[i]);
            return 1;
        } else {
            path = argv[i];
        }
    }
    if (g.mounted) {
        fail(out, "format: a filesystem is already mounted; unmount first");
        return 1;
    }
    err[0] = '\0';
    rc = jfs_format(path, &opts, err, sizeof(err));
    if (rc != JFS_OK) {
        fail(out, "format: %s", err[0] ? err : jfs_strerror(rc));
        return 1;
    }
    jfs_strcpy(g.image, sizeof(g.image), path);
    fprintf(out, "formatted %s (%llu KiB, %u blocks of %u bytes)\n", path,
            (unsigned long long)(opts.size_bytes ? opts.size_bytes : (uint64_t)JFS_DEFAULT_SIZE_KIB * 1024ull) / 1024ull,
            (unsigned)((opts.size_bytes ? opts.size_bytes : (uint64_t)JFS_DEFAULT_SIZE_KIB * 1024ull) / JFS_BLOCK_SIZE),
            JFS_BLOCK_SIZE);
    return 0;
}

static int cmd_mount(int argc, char **argv, FILE *out)
{
    const char *path = (argc > 1) ? argv[1] : g.image;
    const jfs_recovery_stats *st;
    char err[256];
    int rc;

    if (g.mounted) {
        fail(out, "mount: %s is already mounted", g.image);
        return 1;
    }
    err[0] = '\0';
    rc = jfs_mount(path, g.opts.quiet, err, sizeof(err));
    if (rc != JFS_OK) {
        fail(out, "mount: %s", err[0] ? err : jfs_strerror(rc));
        return 1;
    }
    g.mounted = true;
    jfs_strcpy(g.image, sizeof(g.image), path);
    jfs_strcpy(g.cwd, sizeof(g.cwd), "/");

    /* Crash injection may be armed only from the Nth commit onwards. */
    {
        struct jfs_fs *fs = jfs_fs_get();
        fs->crash_after = (int)g.opts.crash_after;
        if (g.opts.checkpoint_interval)
            fs->checkpoint_interval = g.opts.checkpoint_interval;
        if (g.opts.no_journal) {
            fs->journal_enabled = false;
            bool col = jfs_color_enabled(out);
            if (col) {
                fprintf(out, "%s" JFS_SEP_LINE "%s\n"
                             "%swarning:%s %swrite ahead logging is DISABLED (benchmark baseline mode);%s\n"
                             "         this image no longer offers the documented crash guarantee\n"
                             "%s" JFS_SEP_LINE "%s\n",
                        JFS_COLOR_YELLOW, JFS_COLOR_RESET,
                        JFS_COLOR_BRIGHT_YELLOW, JFS_COLOR_RESET, JFS_COLOR_YELLOW, JFS_COLOR_RESET,
                        JFS_COLOR_YELLOW, JFS_COLOR_RESET);
            } else {
                fprintf(out, "warning: write ahead logging is DISABLED (benchmark "
                             "baseline mode); this image no longer offers the "
                             "documented crash guarantee\n");
            }
        }
        if (g.opts.group_commit > 0)
            fs->group_commit_batch = g.opts.group_commit;
        if (g.opts.verbose)
            fprintf(out, "checkpoint interval: %u transaction(s), group commit: %u, journaling %s\n",
                    fs->checkpoint_interval, fs->group_commit_batch, fs->journal_enabled ? "on" : "OFF");
    }

    if (!g.opts.quiet) {
        if (jfs_fs_get()->ckpt.applied_txid > 0) {
            fprintf(out, "mounted %s: %u inodes max, checkpoint seq %llu, applied txid %llu\n",
                    path, jfs_fs_get()->sb.max_inodes,
                    (unsigned long long)jfs_fs_get()->ckpt.seq,
                    (unsigned long long)jfs_fs_get()->ckpt.applied_txid);
        } else {
            fprintf(out, "mounted %s: %u inodes max, log %llu blocks per half, checkpoint seq %llu\n",
                    path, jfs_fs_get()->sb.max_inodes,
                    (unsigned long long)jfs_log_half_blocks(jfs_fs_get()),
                    (unsigned long long)jfs_fs_get()->ckpt.seq);
        }
    }

    st = jfs_last_recovery_stats();
    if (st && (st->txns_committed > 0 || st->txns_incomplete > 0 || st->records_invalid > 0)) {
        if (!g.opts.quiet) {
            bool col = jfs_color_enabled(out);
            if (col) {
                fprintf(out, "%srecovery ran: %llu transaction(s) replayed, %llu incomplete ignored%s\n\n",
                        JFS_COLOR_BRIGHT_GREEN,
                        (unsigned long long)st->txns_committed,
                        (unsigned long long)st->txns_incomplete,
                        JFS_COLOR_RESET);
            } else {
                fprintf(out, "recovery ran: %llu transaction(s) replayed, %llu incomplete ignored\n\n",
                        (unsigned long long)st->txns_committed,
                        (unsigned long long)st->txns_incomplete);
            }
            if (g.opts.verbose)
                jfs_recovery_report(st, emit_to_file, out);
        }
    } else if (!g.opts.quiet) {
        fprintf(out, "recovery: journal window is empty, nothing to redo\n");
    }
    if (!g.opts.quiet && jfs_fs_get()->journal_truncated) {
        /* Never hide this: the window was cut short by a record that cannot
         * be read, so transactions newer than the last one that could be
         * replayed are gone. */
        bool col = jfs_color_enabled(out);
        if (col) {
            fprintf(out,
                    "%s" JFS_SEP_LINE "%s\n"
                    "%swarning:%s damaged journal record at block %llu: the window was "
                    "truncated there, later transactions could not be replayed\n"
                    "%s" JFS_SEP_LINE "%s\n",
                    JFS_COLOR_YELLOW, JFS_COLOR_RESET,
                    JFS_COLOR_BRIGHT_YELLOW, JFS_COLOR_RESET,
                    (unsigned long long)jfs_fs_get()->journal_damaged_block,
                    JFS_COLOR_YELLOW, JFS_COLOR_RESET);
        } else {
            fprintf(out,
                    "warning: damaged journal record at block %llu: the window was "
                    "truncated there, later transactions could not be replayed\n",
                    (unsigned long long)jfs_fs_get()->journal_damaged_block);
        }
    }
    return 0;
}

static int cmd_unmount(int argc, char **argv, FILE *out)
{
    int rc;

    (void)argc;
    (void)argv;
    if (!g.mounted) {
        fail(out, "unmount: no filesystem is mounted");
        return 1;
    }
    rc = jfs_unmount();
    g.mounted = false;
    if (rc != JFS_OK) {
        fail(out, "unmount: %s", jfs_strerror(rc));
        return 1;
    }
    if (!g.opts.quiet)
        fprintf(out, "\nunmounted %s (checkpoint written, image marked clean)\n", g.image);
    else
        fprintf(out, "unmounted %s (checkpoint written, image marked clean)\n", g.image);
    return 0;
}

/* ------------------------------------------------------------------ */
/* commands: files                                                     */
/* ------------------------------------------------------------------ */

static int cmd_create(int argc, char **argv, FILE *out)
{
    char path[1024];
    uint32_t mode = 0;
    int rc;

    if (argc < 2) {
        fail(out, "%s: missing file name", argv[0]);
        return 1;
    }
    if (argc > 2 && jfs_parse_mode(argv[2], &mode) != JFS_OK) {
        fail(out, "%s: bad mode '%s'", argv[0], argv[2]);
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "%s: invalid path '%s'", argv[0], argv[1]);
        return 1;
    }
    rc = jfs_create(path, mode);
    if (rc == JFS_ERR_EXISTS && strcmp(argv[0], "touch") == 0) {
        fprintf(out, "touched %s\n", path);
        return 0;
    }
    if (rc != JFS_OK)
        return report(rc, argv[0], out);
    fprintf(out, "created %s (%s)\n", path, mode ? argv[2] : "0644");
    return 0;
}

static int cmd_nano(int argc, char **argv, FILE *out)
{
    char path[1024];
    jfs_buf b;
    int rc;
    char tmp_path[] = "/tmp/jfs_nano_XXXXXX";
    int fd;
    const char *editor;
    char cmd[1024];
    bool is_interactive;
    FILE *fp;
    long fsize;
    char *new_data;

    if (argc < 2) {
        fail(out, "%s: missing file name", argv[0]);
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "%s: invalid path '%s'", argv[0], argv[1]);
        return 1;
    }
    if (!g.mounted) {
        fail(out, "%s: no filesystem is mounted", argv[0]);
        return 1;
    }

    jfs_buf_init(&b);
    rc = jfs_read(path, &b);
    if (rc != JFS_OK && rc != JFS_ERR_NOTFOUND) {
        jfs_buf_free(&b);
        return report(rc, argv[0], out);
    }
    bool file_exists = (rc == JFS_OK);

    fd = mkstemp(tmp_path);
    if (fd < 0) {
        jfs_buf_free(&b);
        fail(out, "%s: cannot create temporary file: %s", argv[0], strerror(errno));
        return 1;
    }

    if (file_exists && b.data && b.len > 0) {
        ssize_t written = write(fd, b.data, b.len);
        (void)written;
    }
    close(fd);
    jfs_buf_free(&b);

    is_interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (is_interactive) {
        editor = getenv("EDITOR");
        if (!editor || !*editor) {
            if (access("/usr/bin/nano", X_OK) == 0)
                editor = "/usr/bin/nano";
            else if (access("/bin/nano", X_OK) == 0)
                editor = "/bin/nano";
            else
                editor = "nano";
        }
        snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor, tmp_path);
        int sys_ret = system(cmd);
        if (sys_ret == -1) {
            unlink(tmp_path);
            fail(out, "%s: failed to run editor: %s", argv[0], strerror(errno));
            return 1;
        }
    }

    fp = fopen(tmp_path, "rb");
    if (!fp) {
        unlink(tmp_path);
        fail(out, "%s: cannot read temporary file", argv[0]);
        return 1;
    }
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize < 0)
        fsize = 0;

    new_data = malloc((size_t)fsize + 1);
    if (!new_data && fsize > 0) {
        fclose(fp);
        unlink(tmp_path);
        fail(out, "%s: memory allocation failed", argv[0]);
        return 1;
    }
    if (fsize > 0) {
        size_t r = fread(new_data, 1, (size_t)fsize, fp);
        (void)r;
    }
    if (new_data)
        new_data[fsize] = '\0';
    fclose(fp);
    unlink(tmp_path);

    if (!file_exists) {
        rc = jfs_create(path, 0644);
        if (rc != JFS_OK) {
            free(new_data);
            return report(rc, argv[0], out);
        }
    }

    rc = jfs_write(path, new_data ? new_data : "", (size_t)fsize, 0, false, true);
    free(new_data);
    if (rc != JFS_OK)
        return report(rc, argv[0], out);

    fprintf(out, "wrote %s (%ld byte%s)\n", path, fsize, fsize == 1 ? "" : "s");
    return 0;
}

static int cmd_write(int argc, char **argv, FILE *out)
{
    char path[1024];
    char *text;
    bool append = (strcmp(argv[0], "append") == 0);
    int rc;

    if (argc < 2) {
        fail(out, "%s: missing file name", argv[0]);
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "%s: invalid path '%s'", argv[0], argv[1]);
        return 1;
    }
    text = text_argument(argc, argv, 2);
    if (!text) {
        fail(out, "%s: missing text", argv[0]);
        return 1;
    }
    rc = jfs_write(path, text, strlen(text), 0, append, !append);
    free(text);
    if (rc != JFS_OK)
        return report(rc, argv[0], out);
    fprintf(out, "%s %s\n", append ? "appended to" : "wrote", path);
    return 0;
}

static int cmd_read(int argc, char **argv, FILE *out)
{
    jfs_buf b;
    char path[1024];
    bool raw = (strcmp(argv[0], "cat") == 0);
    int rc;

    if (argc < 2) {
        fail(out, "%s: missing file name", argv[0]);
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "%s: invalid path '%s'", argv[0], argv[1]);
        return 1;
    }
    jfs_buf_init(&b);
    rc = jfs_read(path, &b);
    if (rc != JFS_OK) {
        jfs_buf_free(&b);
        return report(rc, argv[0], out);
    }
    bool col = jfs_color_enabled(out);
    if (raw) {
        if (col)
            fprintf(out, "%s", JFS_COLOR_CYAN);
        fwrite(b.data ? b.data : "", 1, b.len, out);
        if (col)
            fprintf(out, "%s", JFS_COLOR_RESET);
        if (b.len == 0 || b.data[b.len - 1] != '\n')
            fputc('\n', out);
    } else {
        if (col) {
            fprintf(out, "%s" JFS_SEP_SHORT "%s\n", JFS_COLOR_SKY_BLUE, JFS_COLOR_RESET);
            fprintf(out, "  %s[ %llu bytes in %s ]%s\n", JFS_COLOR_PINK, (unsigned long long)b.len, path, JFS_COLOR_RESET);
            fprintf(out, "%s", JFS_COLOR_MINT);
            fwrite(b.data ? b.data : "", 1, b.len, out);
            fprintf(out, "%s", JFS_COLOR_RESET);
            if (b.len == 0 || b.data[b.len - 1] != '\n')
                fputc('\n', out);
            fprintf(out, "%s" JFS_SEP_SHORT "%s\n", JFS_COLOR_SKY_BLUE, JFS_COLOR_RESET);
        } else {
            fprintf(out, "%llu bytes in %s:\n", (unsigned long long)b.len, path);
            fwrite(b.data ? b.data : "", 1, b.len, out);
            if (b.len == 0 || b.data[b.len - 1] != '\n')
                fputc('\n', out);
        }
    }
    jfs_buf_free(&b);
    return 0;
}

struct ls_ctx {
    FILE *out;
    unsigned count;
};

static void ls_emit(void *ud, const char *name, const jfs_dirent *de)
{
    struct ls_ctx *ctx = (struct ls_ctx *)ud;
    struct jfs_fs *fs = jfs_fs_get();
    jfs_inode inode;
    char mode[10];
    bool col = jfs_color_enabled(ctx->out);

    if (jfs_inode_load(fs, de->ino, &inode) != JFS_OK) {
        fprintf(ctx->out, "? %-16s (inode %u is unreadable)\n", name, de->ino);
        ctx->count++;
        return;
    }
    jfs_mode_string(inode.mode, mode, sizeof(mode));
    if (col) {
        const char *name_col = (inode.type == JFS_TYPE_DIR) ? JFS_COLOR_CYAN : JFS_COLOR_GREEN;
        fprintf(ctx->out, "%s%c %s%s %8llu  %s%-16s%s %sino=%u%s\n",
                JFS_COLOR_NORM_GREEN, inode.type == JFS_TYPE_DIR ? 'd' : '-', mode, JFS_COLOR_RESET,
                (unsigned long long)inode.size,
                name_col, name, JFS_COLOR_RESET,
                JFS_COLOR_NORM_GREEN, inode.ino, JFS_COLOR_RESET);
    } else {
        fprintf(ctx->out, "%c %s %8llu  %-16s ino=%u\n",
                inode.type == JFS_TYPE_DIR ? 'd' : '-', mode,
                (unsigned long long)inode.size, name, inode.ino);
    }
    ctx->count++;
}

static int cmd_ls(int argc, char **argv, FILE *out)
{
    char path[1024];
    struct ls_ctx ctx;
    int rc;

    ctx.out = out;
    ctx.count = 0;
    if (argc > 1) {
        if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
            fail(out, "ls: invalid path '%s'", argv[1]);
            return 1;
        }
    } else {
        jfs_strcpy(path, sizeof(path), g.cwd);
    }
    rc = jfs_readdir(path, ls_emit, &ctx);
    if (rc != JFS_OK)
        return report(rc, "ls", out);
    fprintf(out, "%u entr%s\n", ctx.count, ctx.count == 1 ? "y" : "ies");
    return 0;
}

static int cmd_stat(int argc, char **argv, FILE *out)
{
    char path[1024];
    char mode[10], ctime[32], mtime[32];
    jfs_inode inode;
    uint32_t i;
    int rc;

    if (argc < 2) {
        fail(out, "stat: missing file name");
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "stat: invalid path '%s'", argv[1]);
        return 1;
    }
    rc = jfs_stat(path, &inode);
    if (rc != JFS_OK)
        return report(rc, "stat", out);

    jfs_mode_string(inode.mode, mode, sizeof(mode));
    jfs_time_str(&inode.ctime, ctime, sizeof(ctime));
    jfs_time_str(&inode.mtime, mtime, sizeof(mtime));
    bool col = jfs_color_enabled(out);
    if (col) {
        fprintf(out, "%s" JFS_SEP_SHORT "%s\n", JFS_COLOR_PURPLE, JFS_COLOR_RESET);
        fprintf(out, "  %sFile:%s %s%s%s\n", JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_SKY_BLUE, path, JFS_COLOR_RESET);
        fprintf(out, "  %sType:%s %s%-14s%s   %sInode:%s %s%-6u%s   %sGeneration:%s %-5u   %sLinks:%s %u\n",
                JFS_COLOR_BOLD, JFS_COLOR_RESET,
                inode.type == JFS_TYPE_DIR ? JFS_COLOR_PINK : JFS_COLOR_MINT,
                inode.type == JFS_TYPE_DIR ? "directory" : "regular file", JFS_COLOR_RESET,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_PURPLE, inode.ino, JFS_COLOR_RESET,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, inode.generation,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, inode.linkcount);
        fprintf(out, "  %sSize:%s %s%-8llu%s   %sBlocks:%s %-5u   %sMode:%s %s%c%s%s (0%o)\n",
                JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_GOLD, (unsigned long long)inode.size, JFS_COLOR_RESET,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, inode.nblocks,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_GREEN,
                inode.type == JFS_TYPE_DIR ? 'd' : '-', mode, JFS_COLOR_RESET, inode.mode);
        fprintf(out, "   %sUid:%s %-6u    %sGid:%s %-6u    %sFlags:%s 0x%x\n",
                JFS_COLOR_BOLD, JFS_COLOR_RESET, inode.uid,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, inode.gid,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, inode.flags);
        fprintf(out, "%sCreate:%s %s%s%s\n   %sMod:%s %s%s%s\n",
                JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_LIGHT_GRAY, ctime, JFS_COLOR_RESET,
                JFS_COLOR_BOLD, JFS_COLOR_RESET, JFS_COLOR_LIGHT_GRAY, mtime, JFS_COLOR_RESET);
        for (i = 0; i < inode.nblocks; i++) {
            fprintf(out, "  %sData block %u:%s log address %s0x%016llx%s, %s%u byte(s)%s\n",
                    JFS_COLOR_BOLD, i, JFS_COLOR_RESET,
                    JFS_COLOR_GOLD, (unsigned long long)inode.addrs[i], JFS_COLOR_RESET,
                    JFS_COLOR_MINT, inode.blklen[i], JFS_COLOR_RESET);
        }
        fprintf(out, "%s" JFS_SEP_SHORT "%s\n", JFS_COLOR_PURPLE, JFS_COLOR_RESET);
    } else {
        fprintf(out, "  File: %s\n", path);
        fprintf(out, "  Type: %s   Inode: %u   Generation: %u   Links: %u\n",
                inode.type == JFS_TYPE_DIR ? "directory" : "regular file",
                inode.ino, inode.generation, inode.linkcount);
        fprintf(out, "  Size: %-8llu Blocks: %-3u  Mode: %c%s (0%o)\n",
                (unsigned long long)inode.size, inode.nblocks,
                inode.type == JFS_TYPE_DIR ? 'd' : '-', mode, inode.mode);
        fprintf(out, "   Uid: %u   Gid: %u   Flags: 0x%x\n", inode.uid, inode.gid, inode.flags);
        fprintf(out, "Create: %s\n   Mod: %s\n", ctime, mtime);
        for (i = 0; i < inode.nblocks; i++) {
            fprintf(out, "  Data block %u: log address 0x%016llx, %u byte(s)\n",
                    i, (unsigned long long)inode.addrs[i], inode.blklen[i]);
        }
    }
    return 0;
}

static int cmd_rm(int argc, char **argv, FILE *out)
{
    char path[1024];
    bool recursive = false;
    const char *arg = NULL;
    int i, rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "-rf") == 0)
            recursive = true;
        else
            arg = argv[i];
    }
    if (!arg) {
        fail(out, "%s: missing file name", argv[0]);
        return 1;
    }
    if (to_path(arg, path, sizeof(path)) != JFS_OK) {
        fail(out, "%s: invalid path '%s'", argv[0], arg);
        return 1;
    }
    rc = jfs_rm(path, recursive);
    if (rc != JFS_OK)
        return report(rc, argv[0], out);
    fprintf(out, "removed %s\n", path);
    return 0;
}

static int cmd_mkdir(int argc, char **argv, FILE *out)
{
    char path[1024];
    uint32_t mode = 0;
    int rc;

    if (argc < 2) {
        fail(out, "%s: missing path", argv[0]);
        return 1;
    }
    if (argc > 2 && jfs_parse_mode(argv[2], &mode) != JFS_OK) {
        fail(out, "%s: bad mode '%s'", argv[0], argv[2]);
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "%s: invalid path '%s'", argv[0], argv[1]);
        return 1;
    }
    rc = jfs_mkdir(path, mode);
    if (rc != JFS_OK)
        return report(rc, argv[0], out);
    fprintf(out, "created directory %s\n", path);
    return 0;
}

static int cmd_rmdir(int argc, char **argv, FILE *out)
{
    char path[1024];
    int rc;

    if (argc < 2) {
        fail(out, "%s: missing path", argv[0]);
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "%s: invalid path '%s'", argv[0], argv[1]);
        return 1;
    }
    rc = jfs_rmdir(path);
    if (rc != JFS_OK)
        return report(rc, argv[0], out);
    fprintf(out, "removed directory %s\n", path);
    return 0;
}

static int cmd_rename(int argc, char **argv, FILE *out)
{
    char from[1024], to[1024];
    int rc;

    if (argc < 3) {
        fail(out, "rename: usage: rename OLD NEW");
        return 1;
    }
    if (to_path(argv[1], from, sizeof(from)) != JFS_OK ||
        to_path(argv[2], to, sizeof(to)) != JFS_OK) {
        fail(out, "rename: invalid path");
        return 1;
    }
    rc = jfs_rename(from, to);
    if (rc != JFS_OK)
        return report(rc, "rename", out);
    fprintf(out, "renamed %s -> %s\n", from, to);
    return 0;
}

static int cmd_truncate(int argc, char **argv, FILE *out)
{
    char path[1024];
    uint64_t size;
    int rc;

    if (argc < 3) {
        fail(out, "truncate: usage: truncate FILE SIZE");
        return 1;
    }
    if (jfs_parse_u64(argv[2], &size) != JFS_OK) {
        fail(out, "truncate: bad size '%s'", argv[2]);
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "truncate: invalid path '%s'", argv[1]);
        return 1;
    }
    rc = jfs_truncate(path, size);
    if (rc != JFS_OK)
        return report(rc, "truncate", out);
    fprintf(out, "%s is now %llu bytes\n", path, (unsigned long long)size);
    return 0;
}

static int cmd_chmod(int argc, char **argv, FILE *out)
{
    char path[1024];
    uint32_t mode;
    int rc;

    if (argc < 3) {
        fail(out, "chmod: usage: chmod MODE FILE");
        return 1;
    }
    if (jfs_parse_mode(argv[1], &mode) != JFS_OK) {
        fail(out, "chmod: bad mode '%s'", argv[1]);
        return 1;
    }
    if (to_path(argv[2], path, sizeof(path)) != JFS_OK) {
        fail(out, "chmod: invalid path '%s'", argv[2]);
        return 1;
    }
    rc = jfs_chmod(path, mode);
    if (rc != JFS_OK)
        return report(rc, "chmod", out);
    fprintf(out, "mode of %s is now 0%o\n", path, mode);
    return 0;
}

/* ------------------------------------------------------------------ */
/* commands: shell and diagnostics                                     */
/* ------------------------------------------------------------------ */

/* Test/benchmark helper: create a file of a known size without typing it. */
static int cmd_fill(int argc, char **argv, FILE *out)
{
    char path[1024];
    unsigned char *buf;
    uint64_t size;
    char pattern = 'x';
    int rc;

    if (argc < 3) {
        fail(out, "fill: usage: fill FILE SIZE [CHAR]");
        return 1;
    }
    if (jfs_parse_size(argv[2], &size) != JFS_OK) {
        fail(out, "fill: bad size '%s'", argv[2]);
        return 1;
    }
    if (size > JFS_MAX_FILE_BYTES) {
        fail(out, "fill: %llu bytes exceeds the maximum file size (%u)",
             (unsigned long long)size, JFS_MAX_FILE_BYTES);
        return 1;
    }
    if (argc > 3 && argv[3][0])
        pattern = argv[3][0];
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "fill: invalid path '%s'", argv[1]);
        return 1;
    }
    buf = (unsigned char *)malloc(size ? (size_t)size : 1);
    if (!buf)
        return 1;
    memset(buf, pattern, (size_t)size);
    rc = jfs_write(path, buf, (size_t)size, 0, false, true);
    free(buf);
    if (rc != JFS_OK)
        return report(rc, "fill", out);
    fprintf(out, "filled %s with %llu byte(s) of '%c'\n", path,
            (unsigned long long)size, pattern);
    return 0;
}

static int cmd_hash(int argc, char **argv, FILE *out)
{
    jfs_buf b;
    char path[1024];
    int rc;

    if (argc < 2) {
        fail(out, "hash: missing file name");
        return 1;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "hash: invalid path '%s'", argv[1]);
        return 1;
    }
    jfs_buf_init(&b);
    rc = jfs_read(path, &b);
    if (rc != JFS_OK) {
        jfs_buf_free(&b);
        return report(rc, "hash", out);
    }
    fprintf(out, "crc32c 0x%08x  %llu byte(s)  %s\n", jfs_crc32c(b.data, b.len),
            (unsigned long long)b.len, path);
    jfs_buf_free(&b);
    return 0;
}

static int cmd_cd(int argc, char **argv, FILE *out)
{
    char path[1024];
    uint32_t ino = 0;
    jfs_inode inode;
    int rc;

    if (argc < 2) {
        jfs_strcpy(g.cwd, sizeof(g.cwd), "/");
        return 0;
    }
    if (to_path(argv[1], path, sizeof(path)) != JFS_OK) {
        fail(out, "cd: invalid path '%s'", argv[1]);
        return 1;
    }
    rc = jfs_resolve(path, &ino, NULL, NULL, 0);
    if (rc != JFS_OK)
        return report(rc, "cd", out);
    rc = jfs_inode_load(jfs_fs_get(), ino, &inode);
    if (rc != JFS_OK)
        return report(rc, "cd", out);
    if (inode.type != JFS_TYPE_DIR) {
        fail(out, "cd: not a directory: %s", path);
        return 1;
    }
    jfs_strcpy(g.cwd, sizeof(g.cwd), path);
    return 0;
}

static int cmd_pwd(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    fprintf(out, "%s\n", g.cwd);
    return 0;
}

static int cmd_df(int argc, char **argv, FILE *out)
{
    uint64_t total = 0, used = 0, inodes = 0, maxino = 0, logfree = 0;
    int rc;

    (void)argc;
    (void)argv;
    rc = jfs_df(&total, &used, &inodes, &maxino, &logfree);
    if (rc != JFS_OK)
        return report(rc, "df", out);
    bool col = jfs_color_enabled(out);
    if (col) {
        fprintf(out, "%s" JFS_SEP_SHORT "%s\n", JFS_COLOR_PURPLE, JFS_COLOR_RESET);
        fprintf(out, "  %s◆ JournalFS Storage Capacity%s\n", JFS_COLOR_BOLD, JFS_COLOR_RESET);
        fprintf(out, "    Total Volume:   %s%llu KiB%s\n", JFS_COLOR_WHITE, (unsigned long long)(total / 1024), JFS_COLOR_RESET);
        fprintf(out, "    Live Data Used: %s%llu KiB%s\n", JFS_COLOR_AMBER, (unsigned long long)(used / 1024), JFS_COLOR_RESET);
        fprintf(out, "    Journal In Use: %s%llu KiB%s\n", JFS_COLOR_SKY_BLUE, (unsigned long long)(jfs_fs_get()->ckpt.journal_used * JFS_BLOCK_SIZE / 1024), JFS_COLOR_RESET);
        fprintf(out, "    Inodes Used:    %s%llu%s of %llu\n", JFS_COLOR_PINK, (unsigned long long)inodes, JFS_COLOR_RESET, (unsigned long long)maxino);
        fprintf(out, "    Log Free Space: %s%llu%s block(s)\n", JFS_COLOR_MINT, (unsigned long long)logfree, JFS_COLOR_RESET);
        fprintf(out, "%s" JFS_SEP_SHORT "%s\n", JFS_COLOR_PURPLE, JFS_COLOR_RESET);
    } else {
        fprintf(out, "JournalFS: %llu KiB total, %llu KiB used by live data, "
                     "%llu KiB journal in use\n",
                (unsigned long long)(total / 1024), (unsigned long long)(used / 1024),
                (unsigned long long)(jfs_fs_get()->ckpt.journal_used * JFS_BLOCK_SIZE / 1024));
        fprintf(out, "inodes: %llu of %llu used; log half has %llu free block(s)\n",
                (unsigned long long)inodes, (unsigned long long)maxino,
                (unsigned long long)logfree);
    }
    return 0;
}

static int cmd_stats(int argc, char **argv, FILE *out)
{
    struct jfs_fs *fs = jfs_fs_get();

    (void)argc;
    (void)argv;
    if (!g.mounted) {
        fail(out, "stats: no filesystem is mounted");
        return 1;
    }
    fprintf(out, "counter,value\n");
    fprintf(out, "txns_committed,%llu\n", (unsigned long long)fs->stats.txns_committed);
    fprintf(out, "txns_replayed,%llu\n", (unsigned long long)fs->stats.txns_replayed);
    fprintf(out, "redo_ops,%llu\n", (unsigned long long)fs->stats.redo_ops);
    fprintf(out, "bytes_written,%llu\n", (unsigned long long)fs->stats.bytes_written);
    fprintf(out, "syncs,%llu\n", (unsigned long long)fs->stats.syncs);
    fprintf(out, "compactions,%llu\n", (unsigned long long)fs->stats.compactions);
    fprintf(out, "journal_blocks_freed,%llu\n",
            (unsigned long long)fs->stats.journal_blocks_freed);
    fprintf(out, "checkpoint_seq,%llu\n", (unsigned long long)fs->stats.checkpoint_seq);
    fprintf(out, "journal_used_blocks,%llu\n", (unsigned long long)fs->ckpt.journal_used);
    fprintf(out, "log_head_blocks,%llu\n", (unsigned long long)fs->ckpt.log_head);
    fprintf(out, "log_live_bytes,%llu\n", (unsigned long long)fs->log_live_bytes);
    fprintf(out, "inodes_used,%u\n", fs->imap.count);
    fprintf(out, "applied_txid,%llu\n", (unsigned long long)fs->ckpt.applied_txid);

    jfs_bcache_stats bstats;
    jfs_bcache_get_stats(jfs_get_bcache(), &bstats);
    fprintf(out, "bcache_hits,%llu\n", (unsigned long long)bstats.hits);
    fprintf(out, "bcache_misses,%llu\n", (unsigned long long)bstats.misses);
    fprintf(out, "bcache_evictions,%llu\n", (unsigned long long)bstats.evictions);
    fprintf(out, "bcache_writebacks,%llu\n", (unsigned long long)bstats.writebacks);
    fprintf(out, "bcache_flushes,%llu\n", (unsigned long long)bstats.flushes);
    fprintf(out, "group_commit_batch,%u\n", fs->group_commit_batch);
    fprintf(out, "group_commits,%llu\n", (unsigned long long)fs->stats.group_commits);
    fprintf(out, "group_commit_flushes,%llu\n", (unsigned long long)fs->stats.group_commit_flushes);
    return 0;
}

/* Read an image without mounting it: useful for damaged images. */
static int cmd_inspect(int argc, char **argv, FILE *out)
{
    struct jfs_fs fs;
    char err[256];
    uint64_t size = 0;
    jfs_checkpoint ck;
    int slot = -1;
    int rc, fd;

    if (argc < 2) {
        fail(out, "inspect: usage: inspect PATH");
        return 1;
    }
    memset(&fs, 0, sizeof(fs));
    fd = jfs_image_open(argv[1], err, sizeof(err));
    if (fd < 0) {
        fail(out, "inspect: %s", err);
        return 1;
    }
    fs.fd = fd;
    jfs_strcpy(fs.path, sizeof(fs.path), argv[1]);
    rc = jfs_image_size(fd, &size, err, sizeof(err));
    if (rc == JFS_OK)
        rc = jfs_read_superblock(&fs, err, sizeof(err));
    if (rc == JFS_OK)
        rc = jfs_validate_superblock(&fs.sb, size, err, sizeof(err));
    if (rc != JFS_OK) {
        fail(out, "inspect: %s", err[0] ? err : jfs_strerror(rc));
        close(fd);
        return 1;
    }
    fprintf(out, "image: %s (%llu bytes, %llu blocks, image id %u, flags 0x%x%s)\n",
            argv[1], (unsigned long long)size,
            (unsigned long long)fs.sb.total_blocks, fs.sb.image_id, fs.sb.flags,
            (fs.sb.flags & JFS_SB_CLEAN) ? " CLEAN" : " IN USE");
    for (slot = 0; slot < 2; slot++) {
        rc = jfs_checkpoint_load_slot(&fs, slot, &ck);
        if (rc != JFS_OK)
            fprintf(out, "checkpoint slot %d: unusable (%s)\n", slot, jfs_strerror(rc));
        else
            fprintf(out, "checkpoint slot %d: seq=%llu applied_txid=%llu log_half=%u "
                         "log_head=%llu imap=0x%llx journal_start=%llu used=%u\n",
                    slot, (unsigned long long)ck.seq,
                    (unsigned long long)ck.applied_txid, ck.log_half,
                    (unsigned long long)ck.log_head, (unsigned long long)ck.imap_addr,
                    (unsigned long long)ck.journal_start, ck.journal_used);
    }
    if (jfs_checkpoint_load_best(&fs, &slot, &fs.ckpt) != JFS_OK) {
        fail(out, "inspect: neither checkpoint slot is usable");
        close(fd);
        return 1;
    }
    fs.next_txid = fs.ckpt.next_txid;
    if (jfs_log_read_imap(&fs, fs.ckpt.imap_addr, &fs.imap) != JFS_OK)
        fprintf(out, "inode map at 0x%llx: DAMAGED\n", (unsigned long long)fs.ckpt.imap_addr);
    else
        fprintf(out, "inode map: %u inode(s) in use, next_ino=%u\n",
                fs.imap.count, fs.imap.next_ino);
    jfs_journal_rescan(&fs);
    jfs_journal_dump(&fs, emit_to_file, out);
    close(fd);
    return 0;
}

static int cmd_sync(int argc, char **argv, FILE *out)
{
    struct jfs_fs *fs = jfs_fs_get();
    int rc;

    (void)argc;
    (void)argv;
    rc = jfs_sync();
    if (rc != JFS_OK)
        return report(rc, "sync", out);
    fprintf(out, "synced: checkpoint seq %llu, applied txid %llu, journal window %llu block(s)\n",
            (unsigned long long)fs->ckpt.seq,
            (unsigned long long)fs->ckpt.applied_txid,
            (unsigned long long)fs->ckpt.journal_used);
    return 0;
}

static int cmd_journal(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    if (!g.mounted) {
        fail(out, "journal: no filesystem is mounted");
        return 1;
    }
    jfs_journal_dump(jfs_fs_get(), emit_to_file, out);
    return 0;
}

static int cmd_check(int argc, char **argv, FILE *out)
{
    char err[512];
    int rc;

    (void)argc;
    (void)argv;
    if (!g.mounted) {
        fail(out, "check: no filesystem is mounted");
        return 1;
    }
    err[0] = '\0';
    rc = jfs_check_consistency(jfs_fs_get(), err, sizeof(err));
    if (rc != JFS_OK) {
        fail(out, "check: %s", err[0] ? err : jfs_strerror(rc));
        return 1;
    }
    bool col = jfs_color_enabled(out);
    if (col) {
        fprintf(out, "%s[✓] check: %s%s\n", JFS_COLOR_GREEN, err, JFS_COLOR_RESET);
    } else {
        fprintf(out, "check: %s\n", err);
    }
    return 0;
}

static int cmd_set(int argc, char **argv, FILE *out)
{
    struct jfs_fs *fs = jfs_fs_get();
    uint32_t n;

    if (argc < 3) {
        fail(out, "set: usage: set <checkpoint-interval|group-commit|color> VALUE");
        return 1;
    }
    if (strcmp(argv[1], "color") == 0) {
        if (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "1") == 0 || strcmp(argv[2], "always") == 0) {
            jfs_color_set_override(1);
            fprintf(out, "color = on\n");
        } else if (strcmp(argv[2], "off") == 0 || strcmp(argv[2], "0") == 0 || strcmp(argv[2], "never") == 0) {
            jfs_color_set_override(0);
            fprintf(out, "color = off\n");
        } else if (strcmp(argv[2], "auto") == 0) {
            jfs_color_set_override(-1);
            fprintf(out, "color = auto\n");
        } else {
            fail(out, "set: bad color setting '%s' (use on, off, or auto)", argv[2]);
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "checkpoint-interval") != 0 &&
        strcmp(argv[1], "group-commit") != 0) {
        fail(out, "set: unknown setting '%s'", argv[1]);
        return 1;
    }
    if (jfs_parse_u32(argv[2], &n) != JFS_OK || n == 0) {
        fail(out, "set: bad value '%s'", argv[2]);
        return 1;
    }
    if (!g.mounted) {
        fail(out, "set: no filesystem is mounted");
        return 1;
    }
    if (strcmp(argv[1], "checkpoint-interval") == 0) {
        fs->checkpoint_interval = n;
        fprintf(out, "checkpoint-interval = %u transaction(s)\n", n);
    } else {
        fs->group_commit_batch = n;
        fprintf(out, "group-commit = %u transaction(s)\n", n);
    }
    return 0;
}

static int cmd_help(int argc, char **argv, FILE *out)
{
    size_t i;

    if (argc > 1) {
        for (i = 0; i < NCOMMANDS; i++) {
            if (strcmp(commands[i].name, argv[1]) == 0) {
                fprintf(out, "usage: %s\n       %s\n", commands[i].usage, commands[i].summary);
                return 0;
            }
        }
        fail(out, "help: unknown command '%s'", argv[1]);
        return 1;
    }
    jfs_cli_help(out);
    return 0;
}

static int cmd_echo(int argc, char **argv, FILE *out)
{
    int i;
    for (i = 1; i < argc; i++) {
        fprintf(out, "%s%s", argv[i], (i + 1 < argc) ? " " : "");
    }
    fputc('\n', out);
    return 0;
}

static int cmd_clear(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    fprintf(out, "\033[H\033[2J\033[3J");
    fflush(out);
    return 0;
}

static int cmd_whoami(int argc, char **argv, FILE *out)
{
    const char *user;
    (void)argc;
    (void)argv;
    user = getenv("USER");
    if (!user || !*user)
        user = getenv("LOGNAME");
    if (!user || !*user)
        user = "root";
    fprintf(out, "%s\n", user);
    return 0;
}

static int cmd_history(int argc, char **argv, FILE *out)
{
    int i;
    HIST_ENTRY **list;
    (void)argc;
    (void)argv;
    list = history_list();

    if (list == NULL || list[0] == NULL) {
        fprintf(out, "History is empty.\n");
        return 0;
    }

    for (i = 0; list[i] != NULL; i++) {
        fprintf(out, "%5d  %s\n", i + history_base, list[i]->line);
    }
    return 0;
}

static int cmd_exit(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    (void)out;
    g.exit_requested = true;
    return 0;
}

static int cmd_banner(int argc, char **argv, FILE *out)
{
    (void)argc;
    (void)argv;
    jfs_print_banner(out);
    return 0;
}

void jfs_print_banner(FILE *out)
{
    bool col = jfs_color_enabled(out);
    if (col) {
        fprintf(out,
            "\n"
            "  " JFS_PALETTE_4 "       ██╗ ██████╗ ██╗   ██╗██████╗ ███╗   ██╗ █████╗ ██╗     ███████╗███████╗" JFS_COLOR_RESET "\n"
            "  " JFS_PALETTE_3 "       ██║██╔═══██╗██║   ██║██╔══██╗████╗  ██║██╔══██╗██║     ██╔════╝██╔════╝" JFS_COLOR_RESET "\n"
            "  " JFS_PALETTE_3 "       ██║██║   ██║██║   ██║██████╔╝██╔██╗ ██║███████║██║     █████╗  ███████╗" JFS_COLOR_RESET "\n"
            "  " JFS_PALETTE_2 "  ██   ██║██║   ██║██║   ██║██╔══██╗██║╚██╗██║██╔══██║██║     ██╔══╝  ╚════██║" JFS_COLOR_RESET "\n"
            "  " JFS_PALETTE_5 "  ╚█████╔╝╚██████╔╝╚██████╔╝██║  ██║██║ ╚████║██║  ██║███████╗██║     ███████║" JFS_COLOR_RESET "\n"
            "  " JFS_PALETTE_5 "   ╚════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝╚═╝  ╚═╝╚══════╝╚═╝     ╚══════╝" JFS_COLOR_RESET "\n\n"

            "  %s┌────────────────────────────────────────────────────────────────────────────┐%s\n"
            "  %s│%s  %sJournalFS%s %s—%s %sCrash-Consistent Journaling Filesystem Simulator%s              %s│%s\n"
            "  %s│%s  %sWrite-Ahead Log · Dual Checkpoint Ring · Group Commit · LFS Cleaner%s       %s│%s\n"
            "  %s└────────────────────────────────────────────────────────────────────────────┘%s\n\n"
            "  %s[INFO]%s Type %s'help'%s for categorized command catalog, or %s'exit'%s to quit.\n"
            "  %s[HINT]%s Use Tab for command/file auto-completion, Up/Down for command history.\n"
            "  %s" JFS_SEP_SHORT "%s\n\n",
            JFS_PALETTE_3, JFS_COLOR_RESET,
            JFS_PALETTE_3, JFS_COLOR_RESET,
            JFS_COLOR_WHITE, JFS_COLOR_RESET,
            JFS_COLOR_GRAY, JFS_COLOR_RESET,
            JFS_PALETTE_5, JFS_COLOR_RESET,
            JFS_PALETTE_3, JFS_COLOR_RESET,
            JFS_PALETTE_3, JFS_COLOR_RESET,
            JFS_PALETTE_4, JFS_COLOR_RESET,
            JFS_PALETTE_3, JFS_COLOR_RESET,
            JFS_PALETTE_3, JFS_COLOR_RESET,
            JFS_PALETTE_5, JFS_COLOR_RESET,
            JFS_PALETTE_4, JFS_COLOR_RESET,
            JFS_PALETTE_5, JFS_COLOR_RESET,
            JFS_PALETTE_4, JFS_COLOR_RESET,
            JFS_PALETTE_3, JFS_COLOR_RESET);
    } else {
        fprintf(out,
            "       ██╗ ██████╗ ██╗   ██╗██████╗ ███╗   ██╗ █████╗ ██╗     ███████╗███████╗\n"
            "       ██║██╔═══██╗██║   ██║██╔══██╗████╗  ██║██╔══██╗██║     ██╔════╝██╔════╝\n"
            "       ██║██║   ██║██║   ██║██████╔╝██╔██╗ ██║███████║██║     █████╗  ███████╗\n"
            "  ██   ██║██║   ██║██║   ██║██╔══██╗██║╚██╗██║██╔══██║██║     ██╔══╝  ╚════██║\n"
            "  ╚█████╔╝╚██████╔╝╚██████╔╝██║  ██║██║ ╚████║██║  ██║███████╗██║     ███████║\n"
            "   ╚════╝  ╚═════╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═══╝╚═╝  ╚═╝╚══════╝╚═╝     ╚══════╝\n\n"
            "  JournalFS — Crash-Consistent Journaling Filesystem Simulator\n"
            "  [ WAL Engine · Dual Checkpoints · Group Commit · LFS Cleaner ]\n\n"
            "  Type 'help' for available commands, or 'exit' to quit.\n\n");
    }
    fflush(out);
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */

int jfs_cli_execute(char *line, FILE *out)
{
    char *argv[32];
    int argc;
    size_t i;

    argc = tokenize(line, argv, 32);
    if (argc == 0)
        return 0;
    for (i = 0; i < NCOMMANDS; i++) {
        if (strcmp(commands[i].name, argv[0]) == 0)
            return commands[i].fn(argc, argv, out);
    }
    fail(out, "unknown command '%s' (try 'help')", argv[0]);
    return 1;
}

/* Readline tab auto-completion generator */
static char *command_generator(const char *text, int state)
{
    static size_t list_index, len;
    const char *name;

    if (!state) {
        list_index = 0;
        len = strlen(text);
    }

    while (list_index < NCOMMANDS) {
        name = commands[list_index++].name;
        if (strncmp(name, text, len) == 0)
            return strdup(name);
    }
    return NULL;
}

static char **jfs_cli_completion(const char *text, int start, int end)
{
    (void)end;
    /* If completing the first word on the line, complete command names */
    if (start == 0)
        return rl_completion_matches(text, command_generator);
    /* For parameters (paths, etc.), fall back to readline filename completion */
    return rl_completion_matches(text, rl_filename_completion_function);
}

/* Signal handling for clean interactive terminal restoration */
static volatile sig_atomic_t g_interrupted = 0;
static struct termios g_orig_termios;
static bool g_termios_saved = false;

static void sigint_handler(int sig)
{
    (void)sig;
    g_interrupted = 1;
    if (RL_ISSTATE(RL_STATE_READCMD)) {
        rl_free_line_state();
        rl_cleanup_after_signal();
        RL_UNSETSTATE(RL_STATE_READCMD);
    }
    if (write(STDOUT_FILENO, "\n", 1) < 0) {
        /* ignore */
    }
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

static void sigtstp_handler(int sig)
{
    (void)sig;
    if (RL_ISSTATE(RL_STATE_READCMD)) {
        rl_deprep_terminal();
    }
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    }
    signal(SIGTSTP, SIG_DFL);
    kill(getpid(), SIGTSTP);
}

static void sigcont_handler(int sig)
{
    (void)sig;
    signal(SIGTSTP, sigtstp_handler);
    if (RL_ISSTATE(RL_STATE_READCMD)) {
        rl_prep_terminal(0);
    }
    rl_forced_update_display();
}

int jfs_cli_run_stream(FILE *in, FILE *out)
{
    int before = g.failures;

    /* Interactive TTY session: full GNU readline with history, completion, and two-tier prompt */
    if (isatty(fileno(in)) && out == stdout) {
        struct sigaction sa_int, sa_tstp, sa_cont, old_int, old_tstp, old_cont;
        memset(&sa_int, 0, sizeof(sa_int));
        sa_int.sa_handler = sigint_handler;
        sigaction(SIGINT, &sa_int, &old_int);

        memset(&sa_tstp, 0, sizeof(sa_tstp));
        sa_tstp.sa_handler = sigtstp_handler;
        sigaction(SIGTSTP, &sa_tstp, &old_tstp);

        memset(&sa_cont, 0, sizeof(sa_cont));
        sa_cont.sa_handler = sigcont_handler;
        sigaction(SIGCONT, &sa_cont, &old_cont);

        if (tcgetattr(STDIN_FILENO, &g_orig_termios) == 0) {
            g_termios_saved = true;
        }

        rl_attempted_completion_function = jfs_cli_completion;
        rl_catch_signals = 0; /* Handle SIGINT/SIGTSTP ourselves for reliable terminal restoration */

        while (!g.exit_requested) {
            char prompt[2048];
            bool col = jfs_color_enabled(out);
            const char *img_display = g.image[0] ? g.image : "none";
            const char *cwd_display = g.cwd[0] ? g.cwd : "/";

            if (col) {
                /* Two-tier ShellForge prompt with \001 and \002 wrapping non-printing escapes */
                snprintf(prompt, sizeof(prompt),
                    "\001" JFS_PALETTE_3 "\002╭─[\001" JFS_PALETTE_4 "\002journalfs\001" JFS_PALETTE_3 "\002]─"
                    "(\001" JFS_PALETTE_5 "\002%s:%s\001" JFS_PALETTE_3 "\002)\001\033[0m\002\n"
                    "\001" JFS_PALETTE_3 "\002╰─>>> \001\033[1;37m\002",
                    img_display, cwd_display);
            } else {
                snprintf(prompt, sizeof(prompt),
                    "╭─[journalfs]─(%s:%s)\n"
                    "╰─>>> ",
                    img_display, cwd_display);
            }

            char *input = readline(prompt);
            if (col) {
                fprintf(out, "%s", JFS_COLOR_RESET);
                fflush(out);
            }

            if (!input) {
                /* EOF (Ctrl+D) */
                fprintf(out, "\n");
                break;
            }

            /* Strip leading/trailing whitespace */
            char *p = input;
            while (*p && isspace((unsigned char)*p))
                p++;
            if (*p && *p != '#') {
                add_history(p);
                jfs_cli_execute(p, out);
                fflush(out);
            }
            free(input);
        }

        /* Restore terminal attributes and signal handlers */
        rl_deprep_terminal();
        if (g_termios_saved) {
            tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        }
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGTSTP, &old_tstp, NULL);
        sigaction(SIGCONT, &old_cont, NULL);
        return g.failures - before;
    }

    /* Automated / Script / Pipe non-interactive stream: preserve exact getline behavior */
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    while (!g.exit_requested) {
        n = getline(&line, &cap, in);
        if (n <= 0)
            break;
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (line[0] == '#' || line[0] == '\0')
            continue;
        jfs_cli_execute(line, out);
        fflush(out);
    }
    free(line);
    return g.failures - before;
}

int jfs_cli_parse(int argc, char **argv, jfs_cli_opts *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->color_mode = -1;
    opts->image = getenv("JFS_IMAGE");
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-i") == 0 || strcmp(a, "--image") == 0) {
            if (++i >= argc)
                return JFS_ERR_INVAL;
            opts->image = argv[i];
        } else if (strcmp(a, "-c") == 0 || strcmp(a, "--command") == 0) {
            if (++i >= argc)
                return JFS_ERR_INVAL;
            if (opts->ncommands >= JFS_CLI_MAX_COMMANDS) {
                /* Say what to do instead of failing with a bare usage error. */
                fprintf(stderr, "journalfs: at most %u -c commands are accepted; "
                                "put longer command lists in a file and pass it "
                                "with -f FILE\n", (unsigned)JFS_CLI_MAX_COMMANDS);
                return JFS_ERR_INVAL;
            }
            opts->commands[opts->ncommands++] = argv[i];
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--script") == 0) {
            if (++i >= argc)
                return JFS_ERR_INVAL;
            opts->script = argv[i];
        } else if (strcmp(a, "--crash") == 0) {
            if (++i >= argc)
                return JFS_ERR_INVAL;
            opts->crash = argv[i];
        } else if (strcmp(a, "--crash-after") == 0) {
            if (++i >= argc)
                return JFS_ERR_INVAL;
            {
                uint32_t n;
                if (jfs_parse_u32(argv[i], &n) != JFS_OK)
                    return JFS_ERR_INVAL;
                opts->crash_after = n;
            }
        } else if (strcmp(a, "--quiet") == 0 || strcmp(a, "-q") == 0) {
            opts->quiet = true;
        } else if (strcmp(a, "--verbose") == 0 || strcmp(a, "-v") == 0) {
            opts->verbose = true;
        } else if (strcmp(a, "--no-recover") == 0) {
            opts->no_recover = true;
        } else if (strcmp(a, "--no-journal") == 0) {
            opts->no_journal = true;
        } else if (strcmp(a, "--checkpoint-interval") == 0) {
            uint32_t n;
            if (++i >= argc || jfs_parse_u32(argv[i], &n) != JFS_OK || n == 0)
                return JFS_ERR_INVAL;
            opts->checkpoint_interval = n;
        } else if (strcmp(a, "--group-commit") == 0) {
            uint32_t n;
            if (++i >= argc || jfs_parse_u32(argv[i], &n) != JFS_OK || n == 0)
                return JFS_ERR_INVAL;
            opts->group_commit = n;
        } else if (strcmp(a, "--color") == 0 || strcmp(a, "--color=always") == 0) {
            opts->color_mode = 1;
        } else if (strcmp(a, "--no-color") == 0 || strcmp(a, "--color=never") == 0) {
            opts->color_mode = 0;
        } else if (strcmp(a, "--color=auto") == 0) {
            opts->color_mode = -1;
        } else if (strcmp(a, "--banner") == 0 || strcmp(a, "--logo") == 0) {
            opts->show_banner = true;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            opts->show_help = true;
        } else if (strcmp(a, "--version") == 0) {
            opts->show_version = true;
        } else {
            return JFS_ERR_INVAL;
        }
    }
    return JFS_OK;
}

int jfs_cli_main(int argc, char **argv)
{
    jfs_cli_opts opts;
    int rc;

    memset(&g, 0, sizeof(g));
    jfs_strcpy(g.cwd, sizeof(g.cwd), "/");
    jfs_strcpy(g.image, sizeof(g.image), "journalfs.img");
    jfs_crash_init_from_env();

    rc = jfs_cli_parse(argc, argv, &opts);
    if (rc != JFS_OK) {
        fprintf(stderr, "journalfs: bad command line (try --help)\n");
        return 2;
    }
    g.opts = opts;
    if (opts.image)
        jfs_strcpy(g.image, sizeof(g.image), opts.image);

    if (opts.color_mode != -1)
        jfs_color_set_override(opts.color_mode);

    if (opts.show_banner) {
        jfs_print_banner(stdout);
        return 0;
    }

    if (opts.show_help) {
        fprintf(stdout, "JournalFS -- crash consistent journaling/log structured filesystem simulator\n"
                        "usage: journalfs [-i IMAGE] [-c CMD]... [-f SCRIPT] [--crash POINT]\n\n");
        jfs_cli_help(stdout);
        return 0;
    }
    if (opts.show_version) {
        fprintf(stdout, "JournalFS format version %u, block size %u, max file %u bytes\n",
                JFS_FORMAT_VERSION, JFS_BLOCK_SIZE, JFS_MAX_FILE_BYTES);
        return 0;
    }

    if (opts.crash) {
        jfs_crash_point p = jfs_crash_parse(opts.crash);
        if (p == JFS_CRASH__COUNT) {
            fprintf(stderr, "journalfs: unknown crash point '%s' (try --help)\n", opts.crash);
            return 2;
        }
        jfs_crash_set(p);
    }

    if (opts.ncommands > 0) {
        unsigned k;
        for (k = 0; k < opts.ncommands && !g.exit_requested; k++) {
            char *copy = strdup(opts.commands[k]);
            if (!copy) {
                fprintf(stderr, "journalfs: out of memory\n");
                return 2;
            }
            jfs_cli_execute(copy, stdout);
            free(copy);
        }
    } else if (opts.script) {
        FILE *in = fopen(opts.script, "r");
        if (!in) {
            fprintf(stderr, "journalfs: cannot open script '%s': %s\n",
                    opts.script, strerror(errno));
            return 2;
        }
        jfs_cli_run_stream(in, stdout);
        fclose(in);
    } else {
        if (isatty(fileno(stdin)) && !opts.quiet) {
            jfs_print_banner(stdout);
        }
        jfs_cli_run_stream(stdin, stdout);
    }

    if (g.mounted)
        jfs_unmount();

    return (g.failures > 0) ? 1 : 0;
}
