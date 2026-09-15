/* Based on the CS:APP shellex.c baseline.
 * make -> ./myshell. Complete week1, then extend the same files for week2.
 * myps/mytop, event_wait/event_notify and the week3 server stay external
 * programs; do not add their implementation as shell builtins.
 */
#include "myshell.h"

int main(void)
{
    char cmdline[MAXLINE];
    int interactive = isatty(STDIN_FILENO);

    /* Avoid prefetching input that a foreground child may need to read. */
    setvbuf(stdin, NULL, _IONBF, 0);
    init_signals();
    while (1) {
        reap_children();
        if (interactive) {
            printf("myshell> ");
            fflush(stdout);
        }
        if (fgets(cmdline, sizeof(cmdline), stdin) == NULL) {
            if (feof(stdin))
                break;
            if (errno == EINTR) {
                clearerr(stdin);
                continue;
            }
            perror("fgets");
            return 1;
        }
        if (strlen(cmdline) == MAXLINE - 1 && cmdline[MAXLINE - 2] != '\n') {
            int c = getchar();
            if (c != '\n' && c != EOF) {
                while ((c = getchar()) != '\n' && c != EOF)
                    ;
                fprintf(stderr, "myshell: input too long\n");
                continue;
            }
        }
        eval(cmdline);
    }
    cleanup_jobs();
    return 0;
}

void eval(char *cmdline)
{
    struct command_line line = {0};
    char original[MAXLINE];

    strcpy(original, cmdline);
    if (parseline(cmdline, &line) < 0 || line.count == 0)
        return;
    if (builtin_command(&line))
        return;
    execute(&line, original);
}

int builtin_command(const struct command_line *line)
{
    for (int i = 0; i < line->count; ++i) {
        const char *name = line->commands[i].argv[0];
        if (strcmp(name, "cd") && strcmp(name, "pwd") && strcmp(name, "exit"))
            continue;
        if (line->count != 1 || line->background) {
            fprintf(stderr, "myshell: use builtins as standalone foreground commands\n");
            return 1;
        }
        if (!strcmp(name, "exit")) {
            if (line->commands[i].argc != 1) {
                fprintf(stderr, "usage: exit\n");
                return 1;
            }
            cleanup_jobs();
            exit(0);
        }
        /* TODO week1: cd -> chdir(), pwd -> getcwd(), in the parent.
         * Validate arguments. Report errors without terminating the shell.
         */
        fprintf(stderr, "TODO week1: implement %s\n", name);
        return 1;
    }
    return 0;
}
