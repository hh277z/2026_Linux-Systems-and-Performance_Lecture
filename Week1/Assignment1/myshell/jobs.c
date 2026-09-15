#include "myshell.h"

/* week2 only. No handlers or background behavior are provided yet.
 * Full fg/bg commands and bash-style terminal job control are not required.
 */
struct job jobs[MAXJOBS];

void init_signals(void)
{
    /* TODO week2: sigaction for SIGCHLD/SIGINT.
     * Keep handlers async-signal-safe; no printf or job-table edits here.
     * Define which children receive terminal Ctrl-C.
     */
}

int add_job(const pid_t *pids, int count, int background, const char *command)
{
    /* TODO week2: find a free slot, copy PIDs/text, assign an id.
     * Reserve space before launch; reject a full table without losing PIDs.
     * Return the slot index, or -1 on failure.
     */
    (void)jobs;
    (void)pids;
    (void)count;
    (void)background;
    (void)command;
    return -1;
}

void reap_children(void)
{
    /* TODO week2: drain waitpid(..., WNOHANG), save matching child statuses.
     * Print Done once all job members exit; release the slot.
     * One SIGCHLD can represent multiple exited children.
     * main's fgets/EINTR loop alone is insufficient while idle: implement a
     * race-free notification/wait design and adjust main's input path too.
     */
}

void cleanup_jobs(void)
{
    /* TODO week2: choose an exit/EOF policy for owned background children.
     * Do not signal unrelated processes or wait indefinitely on shutdown.
     */
}
