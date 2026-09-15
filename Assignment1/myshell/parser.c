#include "myshell.h"

/* Provided: ordinary whitespace-separated arguments, with a bounds check.
 * No quoting/expansion is provided. argv points into buf and is temporary.
 * Return 0 on success (count == 0 for blank input), -1 on an error.
 */
int parseline(char *buf, struct command_line *line)
{
    char *save = NULL;
    char *word;
    struct command *cmd;

    memset(line, 0, sizeof(*line));
    if (strpbrk(buf, "\"'")) {
        fprintf(stderr, "myshell: quoting is not supported by this starter\n");
        return -1;
    }
    if (strchr(buf, '|') || strchr(buf, '&')) {
        /* TODO week1: split at '|', fill commands[0] and commands[1].
         * Reject empty sides and more than two commands.
         * TODO week2: remove a trailing '&' and set background = 1.
         * Operators must not reach execvp as ordinary arguments.
         */
        fprintf(stderr, "TODO: pipeline (week1) / background (week2) parsing\n");
        return -1;
    }
    cmd = &line->commands[0];
    for (word = strtok_r(buf, " \t\r\n", &save); word != NULL;
         word = strtok_r(NULL, " \t\r\n", &save)) {
        if (cmd->argc >= MAXARGS - 1) {
            fprintf(stderr, "myshell: too many arguments\n");
            return -1;
        }
        cmd->argv[cmd->argc++] = word;
    }
    cmd->argv[cmd->argc] = NULL;
    line->count = cmd->argc != 0;
    return 0;
}
