#include "myshell.h"

void execute(const struct command_line *line, const char *original)
{
    if (line->background) {
        /* TODO week2: register all child PIDs, then return to the prompt.
         * Coordinate SIGCHLD blocking/registration and child mask restoration.
         * Copy original text: parsed argv expires at the next input.
         */
        fprintf(stderr, "TODO week2: background execution\n");
        return;
    }
    if (line->count == 1) {
        /* TODO week1: fork; child execvp; parent wait_foreground.
         * On exec failure: perror and _exit(nonzero) in the CHILD only.
         */
        fprintf(stderr, "TODO week1: fork / execvp / waitpid\n");
    } else {
        /* TODO week1: pipe; fork two children; connect stdout/stdin with dup2.
         * Close unused FDs everywhere. Launch BOTH children before waiting.
         * Handle partial failures, such as a failed second fork.
         */
        fprintf(stderr, "TODO week1: two-command pipeline\n");
    }
    /* week2: share the launch path for foreground and background commands;
     * only waiting policy differs. Restore child signal behavior before exec.
     */
    (void)original;
}

void wait_foreground(struct job *job)
{
    /* TODO week1: waitpid for every child in this job; handle errors/EINTR.
     * TODO week2: coordinate with reap_children to avoid lost/double waits.
     * Also reap background jobs during a foreground wait.
     */
    (void)job;
}
