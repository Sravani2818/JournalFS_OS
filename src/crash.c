/*
 * crash.c -- deterministic crash injection.
 *
 * The injection is deliberately brutal: `_exit(99)` skips atexit handlers,
 * stdio flushing and every user space buffer.  Whatever is not already in
 * the image (and not handed to the kernel with a completed write) is gone.
 * That is the closest a user space program can get to pulling the power.
 *
 * Note on limits: `fsync()`/`fdatasync()` returning successfully means the
 * *host kernel and device cache* accepted the data.  A simulator cannot
 * verify that the physical platter or NAND cell really holds it; see
 * docs/recovery_design.md for the durability discussion.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "crash.h"
#include "util.h"

static const char *crash_names[] = {
    [JFS_CRASH_NONE]              = "none",
    [JFS_CRASH_PRE_WRITE]         = "pre_write",
    [JFS_CRASH_POST_WRITE]        = "post_write",
    [JFS_CRASH_POST_SYNC]         = "post_sync",
    [JFS_CRASH_POST_COMMIT]       = "post_commit",
    [JFS_CRASH_POST_COMMIT_SYNC]  = "post_commit_sync",
    [JFS_CRASH_POST_APPLY]        = "post_apply",
    [JFS_CRASH_AFTER_IMAP]        = "after_imap",
    [JFS_CRASH_CHECKPOINT_MID]    = "checkpoint_mid",
    [JFS_CRASH_CHECKPOINT_POST]   = "checkpoint_post",
    [JFS_CRASH_COMPACT_MID]       = "compact_mid",
    [JFS_CRASH_COMPACT_POST]      = "compact_post",
    [JFS_CRASH_RECOVER_MID]       = "recover_mid",
    [JFS_CRASH_PRE_FORMAT]        = "pre_format",
};

static jfs_crash_point active_point = JFS_CRASH_NONE;
static unsigned hit_count[JFS_CRASH__COUNT];

jfs_crash_point jfs_crash_parse(const char *name)
{
    int i;

    if (!name || !*name || strcmp(name, "none") == 0)
        return JFS_CRASH_NONE;
    for (i = 0; i < (int)JFS_CRASH__COUNT; i++) {
        if (crash_names[i] && strcmp(crash_names[i], name) == 0)
            return (jfs_crash_point)i;
    }
    return JFS_CRASH__COUNT;
}

const char *jfs_crash_name(jfs_crash_point p)
{
    if (p < 0 || p >= JFS_CRASH__COUNT || !crash_names[p])
        return "invalid";
    return crash_names[p];
}

void jfs_crash_set(jfs_crash_point p)
{
    active_point = p;
}

jfs_crash_point jfs_crash_get(void)
{
    return active_point;
}

unsigned jfs_crash_hits(jfs_crash_point p)
{
    if (p < 0 || p >= JFS_CRASH__COUNT)
        return 0;
    return hit_count[p];
}

void jfs_crash_check(jfs_crash_point p, const char *tag)
{
    if (p >= JFS_CRASH__COUNT)
        return;
    hit_count[p]++;
    if (active_point == JFS_CRASH_NONE || active_point != p)
        return;

    bool col = jfs_color_enabled(stderr);
    if (col) {
        fprintf(stderr,
                "%s" JFS_SEP_SHORT "%s\n"
                "%s[CRASH INJECTION] Triggered at point '%s%s%s%s%s%s'%s\n"
                "%s   Terminating process with _exit(99); uncommitted buffer state discarded.%s\n"
                "%s" JFS_SEP_SHORT "%s\n",
                JFS_COLOR_RED, JFS_COLOR_RESET,
                JFS_COLOR_RED,
                JFS_COLOR_AMBER, jfs_crash_name(p), JFS_COLOR_RED,
                tag ? " (" : "", tag ? tag : "", tag ? ")" : "",
                JFS_COLOR_RESET,
                JFS_COLOR_LIGHT_GRAY, JFS_COLOR_RESET,
                JFS_COLOR_RED, JFS_COLOR_RESET);
    } else {
        fprintf(stderr,
                JFS_SEP_LINE "\n"
                "[crash] injected crash at point '%s'%s%s%s\n"
                "-- terminating with _exit(99); no image write was flushed by this process\n"
                JFS_SEP_LINE "\n",
                jfs_crash_name(p),
                tag ? " (" : "", tag ? tag : "", tag ? ")" : "");
    }

    /* Only the *console* buffers are flushed, so that a test can see where
     * the crash happened.  Bytes for the disk image never travel through
     * stdio (every access uses pwrite/pread), so this does not make anything
     * durable: the image keeps exactly what previous, explicitly synced
     * writes put there. */
    fflush(NULL);
    _exit(99);
}

void jfs_crash_init_from_env(void)
{
    const char *env = getenv("JFS_CRASH");
    jfs_crash_point p;

    if (!env || !*env)
        return;
    p = jfs_crash_parse(env);
    if (p == JFS_CRASH__COUNT) {
        fprintf(stderr, "[crash] ignoring unknown JFS_CRASH='%s'\n", env);
        return;
    }
    jfs_crash_set(p);
}
