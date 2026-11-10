#include "common.h"
#include <fcntl.h>
#include <poll.h>
#include <string.h>

static void display(int fd)
{
    char buffer[1024];

    if (lseek(fd, 0, SEEK_SET) < 0) {
        fail("seek PSI");
    }

    ssize_t length = read(fd, buffer, sizeof buffer - 1);

    if (length < 0) {
        fail("read PSI");
    }

    buffer[length] = '\0';
    printf("monotonic=%.6f\n%s", now(), buffer);
    fflush(stdout);
}


int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[1], "read") && strcmp(argv[1], "trigger"))) {
        fprintf(stderr, "usage: psi_watch read|trigger PATH SECONDS\n");
        return 2;
    }

    int trigger = !strcmp(argv[1], "trigger");
    long seconds = number(argv[3], 1, 60);
    int fd = open(argv[2], (trigger ? O_RDWR : O_RDONLY) | O_NONBLOCK);

    if (fd < 0) {
        fail("open PSI");
    }

    if (trigger) {
        const char condition[] = "some 150000 2000000";

        /* KEY: register on this fd, then retain it for POLLPRI notifications. */
        if (write(fd, condition, sizeof condition - 1) != sizeof condition - 1) {
            fail("register PSI trigger");
        }

    }

    printf("mode=%s path=%s\n", argv[1], argv[2]);
    double end = now() + seconds;

    while (now() < end) {
        if (trigger) {
            struct pollfd event = { .fd = fd, .events = POLLPRI };

            /* KEY: poll sleeps for a kernel event, unlike periodic reading. */
            int count = poll(&event, 1, 500);

            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }

                fail("poll");
            }

            if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "PSI event source unavailable\n");
                close(fd);
                return 1;
            }

            if (event.revents & POLLPRI) {
                puts("TRIGGER");
                display(fd);
            }

        } else {
            display(fd);
            struct timespec delay = { .tv_sec = 1 };
            nanosleep(&delay, NULL);
        }

    }

    /* KEY: closing the descriptor unregisters its trigger. */
    close(fd);
    return 0;
}
