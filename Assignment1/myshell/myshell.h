#ifndef MYSHELL_H
#define MYSHELL_H

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define MAXLINE 8192
#define MAXARGS 128 /* Includes the terminating NULL. */
#define MAXJOBS 32

struct command {
    int argc;
    char *argv[MAXARGS];
};

struct command_line {
    struct command commands[2]; /* Single command or two-command pipeline. */
    int count;
    int background;
};

/* week2: suggested storage; extend as needed.
 * A pipeline is one job with two children; reap both before marking it done.
 */
struct job {
    int used;
    int id;
    int background;
    int count;
    pid_t pids[2];
    int reaped[2];
    int status[2];
    char command[MAXLINE];
};

/* week2: execute.c and jobs.c share child status through this table. */
extern struct job jobs[MAXJOBS];

void eval(char *cmdline);
int parseline(char *buf, struct command_line *line);
int builtin_command(const struct command_line *line);
void execute(const struct command_line *line, const char *original);
void wait_foreground(struct job *job);

/* week2 hooks: no-ops until implemented. */
void init_signals(void);
int add_job(const pid_t *pids, int count, int background, const char *command);
void reap_children(void);
void cleanup_jobs(void);

#endif
