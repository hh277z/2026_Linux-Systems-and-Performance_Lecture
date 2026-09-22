#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t handled;
static void on_int(int sig) {
    (void)sig;
    int saved = errno;
    handled = 1; /* KEY: handler records receipt; it does not terminate. */
    const char msg[] = "HANDLER SIGINT\n";
    ssize_t result = write(STDOUT_FILENO, msg, sizeof msg - 1);
    (void)result;
    errno = saved;
}
static void check(int rc, const char *what) {
    if (rc == -1) { perror(what); exit(EXIT_FAILURE); }
}
static void snapshot(const char *stage) {
    char line[256];
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) { perror("status"); exit(EXIT_FAILURE); }
    printf("\n%s PID=%ld handled=%d\n", stage, (long)getpid(), (int)handled);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "SigPnd:", 7) || !strncmp(line, "ShdPnd:", 7) ||
            !strncmp(line, "SigBlk:", 7) || !strncmp(line, "SigIgn:", 7) ||
            !strncmp(line, "SigCgt:", 7)) fputs(line, stdout);
    }
    fclose(f);
}
static void step(const char *message) {
    puts(message);
    char line[80];
    if (!fgets(line, sizeof line, stdin)) {
        puts("Input closed; exiting."); exit(EXIT_SUCCESS);
    }
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa = {0};
    sigset_t one, empty, previous;
    sigemptyset(&empty);
    check(sigprocmask(SIG_SETMASK, &empty, NULL), "reset mask");
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;
    check(sigaction(SIGINT, &sa, NULL), "reset SIGINT");
    snapshot("S0 DEFAULT");
    step("A: Enter to install handler and block SIGINT. Do not send yet.");
    sa.sa_handler = on_int;
    sa.sa_flags = SA_RESTART;
    check(sigaction(SIGINT, &sa, NULL), "install handler"); /* KEY */
    sigemptyset(&one);
    sigaddset(&one, SIGINT);
    check(sigprocmask(SIG_BLOCK, &one, &previous), "block SIGINT"); /* KEY */
    snapshot("S1 BLOCKED READY");
    step("B: send SIGINT once. A: then Enter to snapshot pending state.");
    snapshot("S2 ONE SENT");
    step("B: send SIGINT twice more. A: then Enter to snapshot again.");
    snapshot("S3 THREE SENT");
    step("A: predict the result, then Enter to restore the previous mask.");
    check(sigprocmask(SIG_SETMASK, &previous, NULL), "restore mask"); /* KEY */
    snapshot("S4 UNBLOCKED");
    step("A: Enter to finish.");
    return 0;
}
