/*
 * cli.h -- Layer 1, the command line interface.
 */
#ifndef JFS_CLI_H
#define JFS_CLI_H

#include <stdbool.h>
#include <stdio.h>

#define JFS_CLI_MAX_COMMANDS 1024

typedef struct {
    const char *image;           /* -i / --image                            */
    const char *commands[JFS_CLI_MAX_COMMANDS];  /* -c / --command, repeatable */
    unsigned    ncommands;
    const char *script;          /* -f / --script                           */
    const char *crash;           /* --crash <point>                         */
    bool        quiet;
    bool        verbose;
    unsigned    crash_after;     /* crash after N durable commits           */
    bool        no_recover;      /* mount without replaying (diagnostics)   */
    bool        no_journal;      /* baseline mode: disable the write ahead log */
    unsigned    checkpoint_interval;  /* 0 = leave the default              */
    unsigned    group_commit;         /* 0 or 1 = per-tx sync, >1 = batch   */
    int         color_mode;           /* -1 = auto, 0 = never, 1 = always   */
    bool        show_banner;          /* --banner / --logo                  */
    bool        show_help;
    bool        show_version;
} jfs_cli_opts;

/* Parse argv.  Returns JFS_OK, or a negative status after printing an error. */
int jfs_cli_parse(int argc, char **argv, jfs_cli_opts *opts);

/* Run commands from a FILE*; returns the number of failed commands. */
int jfs_cli_run_stream(FILE *in, FILE *out);

/* Run one command line (mutates a copy of the buffer). */
int jfs_cli_execute(char *line, FILE *out);

/* Main entry point used by main.c. */
int jfs_cli_main(int argc, char **argv);

/* Print the help text. */
void jfs_cli_help(FILE *out);

/* Print ASCII art logo and banner. */
void jfs_print_banner(FILE *out);

#endif /* JFS_CLI_H */
