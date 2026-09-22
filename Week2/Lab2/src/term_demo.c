#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void on_term(int sig) {
    (void)sig;
    int saved = errno;
    const char msg[] = "HANDLER SIGTERM: returning without exit\n";
    ssize_t result = write(STDOUT_FILENO, msg, sizeof msg - 1);
    (void)result;
    errno = saved;
    /* KEY: returning resumes the interrupted program; no exit request. */
}
static void check(int rc, const char *what) {
    if (rc == -1) { perror(what); exit(EXIT_FAILURE); }
}
int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "default") && strcmp(argv[1], "ignore") &&
                     strcmp(argv[1], "block") && strcmp(argv[1], "catch"))) {
        fprintf(stderr, "Usage: %s default|ignore|block|catch\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    sigset_t mask;
    sigemptyset(&mask);
    check(sigprocmask(SIG_SETMASK, &mask, NULL), "reset mask");
    struct sigaction sa = {0};
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = SIG_DFL;
    if (!strcmp(argv[1], "ignore")) sa.sa_handler = SIG_IGN; /* KEY ignore */
    if (!strcmp(argv[1], "catch")) sa.sa_handler = on_term; /* KEY catch */
    check(sigaction(SIGTERM, &sa, NULL), "sigaction SIGTERM");
    if (!strcmp(argv[1], "block")) {
        sigaddset(&mask, SIGTERM);
        check(sigprocmask(SIG_BLOCK, &mask, NULL), "block SIGTERM"); /* KEY block */
    }
    printf("READY mode=%s PID=%ld SIGTERM=%d\n", argv[1], (long)getpid(), SIGTERM);
    for (;;) pause(); /* KEY: sleep, wake for a handler, then sleep again. */
}
