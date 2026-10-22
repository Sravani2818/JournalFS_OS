/*
 * util.h -- small helpers shared by every layer: status codes, error
 * strings, bounds checked string/number parsing and full read/write loops.
 */
#ifndef JFS_UTIL_H
#define JFS_UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#include "disk_format.h"

/* Status codes.  Zero means success, everything else is an error. */
typedef enum {
    JFS_OK              = 0,
    JFS_ERR_IO          = -1,   /* host read/write/sync failure            */
    JFS_ERR_NOSPACE     = -2,   /* no space left in the log or journal     */
    JFS_ERR_NOTFOUND    = -3,   /* file/directory does not exist           */
    JFS_ERR_EXISTS      = -4,   /* name already exists                     */
    JFS_ERR_INVAL       = -5,   /* malformed argument                      */
    JFS_ERR_CORRUPT     = -6,   /* on-disk data failed validation          */
    JFS_ERR_BADFMT      = -7,   /* image is not a JournalFS image          */
    JFS_ERR_PERM        = -8,   /* permission denied by the simulated model*/
    JFS_ERR_NOTDIR      = -9,   /* path component is not a directory       */
    JFS_ERR_ISDIR       = -10,  /* operation not allowed on a directory    */
    JFS_ERR_NOTEMPTY    = -11,  /* rmdir on a non empty directory          */
    JFS_ERR_NOTMOUNTED  = -12,
    JFS_ERR_MOUNTED     = -13,
    JFS_ERR_NOINODES    = -14,  /* inode table full                        */
    JFS_ERR_JFULL       = -15,  /* journal region full                     */
    JFS_ERR_NAMETOOLONG = -16,
    JFS_ERR_NOTSUP      = -17,
    JFS_ERR_NOMEM       = -18,
    JFS_ERR_FILE_TOO_BIG= -19,
    JFS_ERR_BADPATH     = -20,
    JFS_ERR_DIRFULL     = -21,  /* directory has no room for another entry */
    JFS_ERR_STATE       = -22,  /* operation not valid in this state       */
    JFS_ERR_EXISTSSLOT  = -23,  /* image exists (format without --force)   */
    JFS_ERR_TRUNCATED   = -24,  /* image shorter than the superblock says  */
    JFS_ERR_FAILED      = -25,  /* filesystem stopped after a failed apply  */
    JFS_ERR_REQUIRED    = -26   /* argument is required                     */
} jfs_status;

/* Human readable message for a status code (never NULL). */
const char *jfs_strerror(int status);

/* Full read/write helpers that survive short reads and EINTR. */
ssize_t jfs_pread_full(int fd, void *buf, size_t len, off_t off);
ssize_t jfs_pwrite_full(int fd, const void *buf, size_t len, off_t off);

/* Bounds checked string copy: always NUL terminates, returns JFS_OK or
 * JFS_ERR_INVAL when the source does not fit. */
int jfs_strcpy(char *dst, size_t dstsz, const char *src);

/* Validate a file name for use inside JournalFS (structure AND length). */
bool jfs_valid_name(const char *name);
/* Structural check only, used while normalising a path: a component that is
 * merely too long is reported by the operation, with a message that says so. */
bool jfs_path_component_ok(const char *name);

/* Number parsing with full error checking. */
int jfs_parse_u64(const char *s, uint64_t *out);
int jfs_parse_u32(const char *s, uint32_t *out);
int jfs_parse_size(const char *s, uint64_t *out);   /* "1024", "64k", "2m" */
int jfs_parse_mode(const char *s, uint32_t *out);   /* "644", "0755"       */

/* Current wall clock time. */
void jfs_now(jfs_time *t);

/* Format a jfs_time as "YYYY-MM-DD HH:MM:SS" (UTC). */
void jfs_time_str(const jfs_time *t, char *buf, size_t bufsz);

/* Append a formatted string to a growing in-memory buffer (used by `read`
 * and by the diagnostic commands). */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} jfs_buf;

void jfs_buf_init(jfs_buf *b);
void jfs_buf_free(jfs_buf *b);
int  jfs_buf_append(jfs_buf *b, const void *data, size_t len);
int  jfs_buf_printf(jfs_buf *b, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Split a path into components. Returns a malloc'd array of malloc'd
 * strings; caller frees with jfs_path_free(). */
int  jfs_path_split(const char *path, char ***parts, size_t *nparts);
void jfs_path_free(char **parts, size_t nparts);

/* Normalise a path (resolve . and .. textually) into `out`. */
int  jfs_path_normalize(const char *cwd, const char *in, char *out, size_t outsz);

/* Vibrant Modern Terminal Colors (256-color & truecolor compatible) */
#define JFS_COLOR_RESET        "\033[0m"
#define JFS_COLOR_BOLD         "\033[1m"
#define JFS_COLOR_DIM          "\033[2m"
#define JFS_COLOR_ITALIC       "\033[3m"
#define JFS_COLOR_UNDERLINE    "\033[4m"

/* Custom 5-Color Palette (from user image):
 * C1: Midnight Indigo:  #27104E (RGB: 39, 16, 78)
 * C2: Royal Purple:     #64379F (RGB: 100, 55, 159)
 * C3: Vibrant Violet:   #9854CB (RGB: 152, 84, 203)
 * C4: Soft Lavender:    #DDACF5 (RGB: 221, 172, 245)
 * C5: Electric Cyan:    #75E8E7 (RGB: 117, 232, 231)
 */
#define JFS_PALETTE_1          "\033[38;2;39;16;78m"
#define JFS_PALETTE_2          "\033[38;2;100;55;159m"
#define JFS_PALETTE_3          "\033[38;2;152;84;203m"
#define JFS_PALETTE_4          "\033[38;2;221;172;245m"
#define JFS_PALETTE_5          "\033[38;2;117;232;231m"

/* Vibrant Shades (Mapped to user 5-color palette) */
#define JFS_COLOR_PURPLE       JFS_PALETTE_3
#define JFS_COLOR_VIOLET       JFS_PALETTE_2
#define JFS_COLOR_PINK         JFS_PALETTE_4
#define JFS_COLOR_HOT_PINK     "\033[38;5;198m"
#define JFS_COLOR_CORAL        "\033[38;5;203m"
#define JFS_COLOR_ORANGE       "\033[38;5;208m"
#define JFS_COLOR_AMBER        "\033[38;5;214m"
#define JFS_COLOR_GOLD         "\033[38;5;220m"
#define JFS_COLOR_LIME         "\033[38;5;154m"
#define JFS_COLOR_MINT         "\033[38;5;48m"
#define JFS_COLOR_GREEN        "\033[38;5;84m"
#define JFS_COLOR_CYAN         JFS_PALETTE_5
#define JFS_COLOR_SKY_BLUE     JFS_PALETTE_5
#define JFS_COLOR_BLUE         "\033[38;5;33m"
#define JFS_COLOR_DEEP_BLUE    "\033[38;5;27m"
#define JFS_COLOR_RED          "\033[38;5;196m"
#define JFS_COLOR_DARK_RED     "\033[38;5;124m"
#define JFS_COLOR_GRAY         "\033[38;5;242m"
#define JFS_COLOR_LIGHT_GRAY   "\033[38;5;248m"
#define JFS_COLOR_WHITE        "\033[1;37m"

/* Standard ANSI compatibility fallbacks */
#define JFS_COLOR_YELLOW       "\033[1;33m"
#define JFS_COLOR_MAGENTA      "\033[1;35m"
#define JFS_COLOR_NORM_GREEN   "\033[32m"
#define JFS_COLOR_NORM_CYAN    "\033[36m"
#define JFS_COLOR_NORM_YELLOW  "\033[33m"
#define JFS_COLOR_NORM_RED     "\033[31m"
#define JFS_COLOR_BRIGHT_RED   "\033[91m"
#define JFS_COLOR_BRIGHT_GREEN "\033[92m"
#define JFS_COLOR_BRIGHT_YELLOW "\033[93m"
#define JFS_COLOR_BRIGHT_CYAN  "\033[96m"

#define JFS_SEP_LINE   "────────────────────────────────────────────────────────────────────────────"
#define JFS_SEP_SHORT  "────────────────────────────────────────────────────────────────────────────"
#define JFS_DOUBLE_SEP "════════════════════════════════════════════════════════════════════════════"

bool jfs_color_enabled(FILE *stream);
void jfs_color_set_override(int mode);

#endif /* JFS_UTIL_H */
