/*
 * emmcctl: talk to a running emmcsim through its control socket.
 *
 *   emmcctl stat            counters for people
 *   emmcctl stat raw        the same counters as key=value lines, for scripts
 *   emmcctl reset           zero the counters and the trace
 *   emmcctl trace [clear]   finished die operations and host requests as CSV
 *   emmcctl conf            effective configuration
 *   emmcctl powercut        cut the power now (see Lab 11, exercise 2)
 *
 * The socket is /tmp/emmcsim.ctl unless EMMC_CTL says otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: emmcctl stat [raw] | reset | trace [clear] | "
                "conf | powercut\n");
        return 2;
    }
    char cmd[128] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        }
        strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
    }
    const char *path = getenv("EMMC_CTL");
    if (!path)
        path = "/tmp/emmcsim.ctl";
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr,
                "emmcctl: cannot reach %s "
                "(is emmcsim running and connected?)\n",
                path);
        return 1;
    }
    if (write(fd, cmd, strlen(cmd)) < 0) {
        return 1;
    }
    shutdown(fd, SHUT_WR);
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
    }
    close(fd);
    return 0;
}
