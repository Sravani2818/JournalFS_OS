/*
 * util.c -- helpers shared by every layer.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

const char *jfs_strerror(int status)
{
    switch (status) {
    case JFS_OK:                return "success";
    case JFS_ERR_IO:            return "host I/O error";
    case JFS_ERR_NOSPACE:       return "filesystem is full";
    case JFS_ERR_NOTFOUND:      return "no such file or directory";
    case JFS_ERR_EXISTS:        return "file already exists";
    case JFS_ERR_INVAL:         return "invalid argument";
    case JFS_ERR_CORRUPT:       return "on-disk data failed validation";
    case JFS_ERR_BADFMT:        return "not a JournalFS image (bad magic/version)";
    case JFS_ERR_PERM:          return "permission denied";
    case JFS_ERR_NOTDIR:        return "not a directory";
    case JFS_ERR_ISDIR:         return "is a directory";
    case JFS_ERR_NOTEMPTY:      return "directory not empty";
    case JFS_ERR_NOTMOUNTED:    return "no filesystem is mounted";
    case JFS_ERR_MOUNTED:       return "a filesystem is already mounted";
    case JFS_ERR_NOINODES:      return "inode table is full";
    case JFS_ERR_JFULL:         return "journal region is full";
    case JFS_ERR_NAMETOOLONG:   return "file name too long";
    case JFS_ERR_NOTSUP:        return "operation not supported";
    case JFS_ERR_NOMEM:         return "out of memory";
    case JFS_ERR_FILE_TOO_BIG:  return "file exceeds the maximum size";
    case JFS_ERR_BADPATH:       return "invalid path";
    case JFS_ERR_DIRFULL:       return "directory is full";
    case JFS_ERR_STATE:         return "invalid state for this operation";
    case JFS_ERR_EXISTSSLOT:    return "image already exists (use --force to overwrite)";
    case JFS_ERR_TRUNCATED:     return "image is truncated";
    case JFS_ERR_FAILED:        return "filesystem stopped after a failed update: "
                                       "unmount and mount again to replay it";
    default:                    return "unknown error";
    }
}

ssize_t jfs_pread_full(int fd, void *buf, size_t len, off_t off)
{
    size_t done = 0;
    unsigned char *p = (unsigned char *)buf;

    while (done < len) {
        ssize_t n = pread(fd, p + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;              /* short read: caller decides if that is ok */
        done += (size_t)n;
    }
    return (ssize_t)done;
}

ssize_t jfs_pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
    size_t done = 0;
    const unsigned char *p = (const unsigned char *)buf;

    while (done < len) {
        ssize_t n = pwrite(fd, p + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }
    return (ssize_t)done;
}

int jfs_strcpy(char *dst, size_t dstsz, const char *src)
{
    size_t n;

    if (!dst || dstsz == 0 || !src)
        return JFS_ERR_INVAL;
    n = strlen(src);
    if (n >= dstsz)
        return JFS_ERR_INVAL;
    memcpy(dst, src, n + 1);
    return JFS_OK;
}

bool jfs_path_component_ok(const char *name)
{
    size_t i;

    if (!name || name[0] == '\0')
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    for (i = 0; name[i]; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '/' || c == '\0' || c < 0x20 || c == 0x7F)
            return false;
    }
    return true;
}

bool jfs_valid_name(const char *name)
{
    if (!jfs_path_component_ok(name))
        return false;
    return strlen(name) <= JFS_NAME_MAX;
}

int jfs_parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !*s || !out)
        return JFS_ERR_INVAL;
    if (s[0] == '-' || s[0] == '+')
        return JFS_ERR_INVAL;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return JFS_ERR_INVAL;
    *out = (uint64_t)v;
    return JFS_OK;
}

int jfs_parse_u32(const char *s, uint32_t *out)
{
    uint64_t v;
    int rc = jfs_parse_u64(s, &v);

    if (rc != JFS_OK)
        return rc;
    if (v > 0xFFFFFFFFull)
        return JFS_ERR_INVAL;
    *out = (uint32_t)v;
    return JFS_OK;
}

int jfs_parse_size(const char *s, uint64_t *out)
{
    uint64_t v;
    size_t len;
    char suffix;
    int rc;

    if (!s || !out)
        return JFS_ERR_INVAL;
    len = strlen(s);
    if (len == 0)
        return JFS_ERR_INVAL;
    suffix = s[len - 1];
    if (suffix == 'k' || suffix == 'K' || suffix == 'm' || suffix == 'M') {
        char tmp[64];
        if (len >= sizeof(tmp))
            return JFS_ERR_INVAL;
        memcpy(tmp, s, len - 1);
        tmp[len - 1] = '\0';
        rc = jfs_parse_u64(tmp, &v);
        if (rc != JFS_OK)
            return rc;
        if (suffix == 'k' || suffix == 'K') {
            if (v > UINT64_MAX / 1024ull)
                return JFS_ERR_INVAL;
            v *= 1024ull;
        } else {
            if (v > UINT64_MAX / (1024ull * 1024ull))
                return JFS_ERR_INVAL;
            v *= 1024ull * 1024ull;
        }
    } else {
        rc = jfs_parse_u64(s, &v);
        if (rc != JFS_OK)
            return rc;
    }
    *out = v;
    return JFS_OK;
}

int jfs_parse_mode(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (!s || !*s || !out)
        return JFS_ERR_INVAL;
    errno = 0;
    v = strtoul(s, &end, 8);
    if (errno != 0 || !end || *end != '\0' || v > 0777ul)
        return JFS_ERR_INVAL;
    *out = (uint32_t)v & JFS_PERM_BITS;
    return JFS_OK;
}

void jfs_now(jfs_time *t)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
    }
    t->sec = (int64_t)ts.tv_sec;
    t->nsec = (uint32_t)ts.tv_nsec;
    t->pad = 0;
}

void jfs_time_str(const jfs_time *t, char *buf, size_t bufsz)
{
    time_t sec = (time_t)t->sec;
    struct tm tmv;

    if (gmtime_r(&sec, &tmv) == NULL) {
        snprintf(buf, bufsz, "(invalid time)");
        return;
    }
    snprintf(buf, bufsz, "%04d-%02d-%02d %02d:%02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* ------------------------------------------------------------------ */
/* growing buffer                                                      */
/* ------------------------------------------------------------------ */

void jfs_buf_init(jfs_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void jfs_buf_free(jfs_buf *b)
{
    free(b->data);
    jfs_buf_init(b);
}

static int buf_reserve(jfs_buf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    size_t cap;
    char *p;

    if (need <= b->cap)
        return JFS_OK;
    cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > (size_t)1 << 40)
            return JFS_ERR_NOMEM;
        cap *= 2;
    }
    p = (char *)realloc(b->data, cap);
    if (!p)
        return JFS_ERR_NOMEM;
    b->data = p;
    b->cap = cap;
    return JFS_OK;
}

int jfs_buf_append(jfs_buf *b, const void *data, size_t len)
{
    int rc = buf_reserve(b, len);

    if (rc != JFS_OK)
        return rc;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return JFS_OK;
}

int jfs_buf_printf(jfs_buf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    int n;
    int rc;

    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return JFS_ERR_INVAL;
    }
    rc = buf_reserve(b, (size_t)n);
    if (rc != JFS_OK) {
        va_end(ap2);
        return rc;
    }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return JFS_OK;
}

/* ------------------------------------------------------------------ */
/* paths                                                               */
/* ------------------------------------------------------------------ */

static int push_part(char ***parts, size_t *n, size_t *cap, const char *s, size_t len)
{
    char **np;
    char *copy;

    copy = (char *)malloc(len + 1);
    if (!copy)
        return JFS_ERR_NOMEM;
    memcpy(copy, s, len);
    copy[len] = '\0';

    if (*n == *cap) {
        size_t ncap = *cap ? *cap * 2 : 8;
        np = (char **)realloc(*parts, ncap * sizeof(char *));
        if (!np) {
            free(copy);
            return JFS_ERR_NOMEM;
        }
        *parts = np;
        *cap = ncap;
    }
    (*parts)[(*n)++] = copy;
    return JFS_OK;
}

void jfs_path_free(char **parts, size_t nparts)
{
    size_t i;

    if (!parts)
        return;
    for (i = 0; i < nparts; i++)
        free(parts[i]);
    free(parts);
}

int jfs_path_split(const char *path, char ***parts, size_t *nparts)
{
    char **out = NULL;
    size_t n = 0, cap = 0, i = 0;
    int rc;

    *parts = NULL;
    *nparts = 0;
    if (!path)
        return JFS_ERR_BADPATH;

    while (path[i] != '\0') {
        size_t start;
        while (path[i] == '/')
            i++;
        if (path[i] == '\0')
            break;
        start = i;
        while (path[i] != '\0' && path[i] != '/')
            i++;
        rc = push_part(&out, &n, &cap, path + start, i - start);
        if (rc != JFS_OK) {
            jfs_path_free(out, n);
            return rc;
        }
    }
    *parts = out;
    *nparts = n;
    return JFS_OK;
}

int jfs_path_normalize(const char *cwd, const char *in, char *out, size_t outsz)
{
    char combined[1024];
    char **parts = NULL;
    size_t nparts = 0, i;
    size_t used = 0;

    if (!in || !out || outsz == 0)
        return JFS_ERR_BADPATH;

    if (in[0] == '/') {
        if (jfs_strcpy(combined, sizeof(combined), in) != JFS_OK)
            return JFS_ERR_BADPATH;
    } else {
        const char *base = (cwd && *cwd) ? cwd : "/";
        if (base[0] == '/')
            snprintf(combined, sizeof(combined), "%s/%s", base, in);
        else
            snprintf(combined, sizeof(combined), "/%s/%s", base, in);
    }

    if (jfs_path_split(combined, &parts, &nparts) != JFS_OK)
        return JFS_ERR_NOMEM;

    out[0] = '/';
    out[1] = '\0';
    used = 1;

    /* Rebuild the path, resolving "." and ".." textually. */
    for (i = 0; i < nparts; i++) {
        if (strcmp(parts[i], ".") == 0)
            continue;
        if (strcmp(parts[i], "..") == 0) {
            char *slash = (used > 1) ? strrchr(out, '/') : NULL;
            if (slash)
                *slash = '\0';
            if (out[0] == '\0') {
                out[0] = '/';
                out[1] = '\0';
            }
            used = strlen(out);
            continue;
        }
        if (!jfs_path_component_ok(parts[i])) {
            jfs_path_free(parts, nparts);
            return JFS_ERR_BADPATH;
        }
        {
            size_t need = strlen(parts[i]) + (used > 1 ? 1 : 0);
            if (used + need + 1 > outsz) {
                jfs_path_free(parts, nparts);
                return JFS_ERR_BADPATH;
            }
            if (used > 1)
                out[used++] = '/';
            memcpy(out + used, parts[i], strlen(parts[i]) + 1);
            used += strlen(parts[i]);
        }
    }
    jfs_path_free(parts, nparts);
    if (out[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
    }
    return JFS_OK;
}

static int g_color_override = -1; /* -1 = auto, 0 = never, 1 = always */

void jfs_color_set_override(int mode)
{
    g_color_override = mode;
}

bool jfs_color_enabled(FILE *stream)
{
    if (g_color_override == 0)
        return false;
    if (g_color_override == 1)
        return true;

    const char *no_col = getenv("NO_COLOR");
    if (no_col && *no_col)
        return false;

    const char *jfs_col = getenv("JFS_COLOR");
    if (jfs_col) {
        if (strcmp(jfs_col, "0") == 0 || strcmp(jfs_col, "never") == 0 || strcmp(jfs_col, "no") == 0)
            return false;
        if (strcmp(jfs_col, "1") == 0 || strcmp(jfs_col, "always") == 0 || strcmp(jfs_col, "yes") == 0)
            return true;
    }

    if (!stream)
        return false;

    return isatty(fileno(stream)) != 0;
}

