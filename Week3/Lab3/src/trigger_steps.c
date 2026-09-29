#include "lab.h"
#include <sys/ioctl.h>

static int remaining(int fd) {
    int bytes;
    check(ioctl(fd, FIONREAD, &bytes), "FIONREAD");
    return bytes;
}

static int read_piece(int fd) {
    char buf[3] = {0};
    ssize_t n;
    do {
        n = read(fd, buf, 2); /* KEY_PARTIAL */
    } while (n < 0 && errno == EINTR);

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        puts("READ EAGAIN");
        return -1;
    }

    if (n < 0) {
        fail("read");
    }

    printf("READ n=%zd data=%s\n", n, buf);
    return (int)n;
}

static int wait_ready(int epfd) {
    struct epoll_event ev;
    int n;
    do {
        n = epoll_wait(epfd, &ev, 1, 600); /* KEY_SECOND_WAIT */
    } while (n < 0 && errno == EINTR);

    check(n, "epoll_wait");
    printf("WAIT returned=%d%s\n", n, n ? " READY" : " TIMEOUT");
    return n;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3 ||
        (strcmp(argv[1], "lt") && strcmp(argv[1], "et-bad") && strcmp(argv[1], "et-drain")) ||
        (argc == 3 && strcmp(argv[2], "--auto"))) {
        fprintf(stderr, "Usage: %s lt|et-bad|et-drain [--auto]\n", argv[0]);
        return 2;
    }

    bool edge = strcmp(argv[1], "lt") != 0;
    bool drain = !strcmp(argv[1], "et-drain");
    bool automatic = argc == 3;
    setvbuf(stdout, NULL, _IOLBF, 0);
    int fds[2];
    check(pipe(fds), "pipe");
    nonblock(fds[0]); /* KEY_NONBLOCK */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    check(epfd, "epoll_create1");
    struct epoll_event ev = {.events = EPOLLIN | (edge ? EPOLLET : 0), /* KEY_MODE */
                             .data.fd = fds[0]};
    check(epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev), "epoll_ctl");
    printf("\n========== TRIGGER MODE: %s ==========\n", argv[1]);
    puts("\n--- STEP S0: register the read FD; inspect before writing ---");
    printf("S0 REGISTERED mode=%s PID=%ld epfd=%d readfd=%d writefd=%d remaining=%d\n", argv[1],
           (long)getpid(), epfd, fds[0], fds[1], remaining(fds[0]));
    next_step(automatic);
    puts("\n--- STEP S1: write 8 bytes, wait, then read ---");
    if (write(fds[1], "ABCDEFGH", 8) != 8) { /* KEY_WRITE_ONCE */
        fail("write eight bytes");
    }

    if (wait_ready(epfd) != 1) {
        fprintf(stderr, "Missing initial event\n");
        return 1;
    }

    int total = 0, n;
    if (drain) {
        while ((n = read_piece(fds[0])) > 0) { /* KEY_DRAIN */
            total += n;
        }
    } else {
        n = read_piece(fds[0]);
        if (n > 0) {
            total += n;
        }
    }

    int after_first = remaining(fds[0]);
    printf("S1 AFTER_FIRST total_read=%d remaining=%d\n", total, after_first);
    puts("No new writes or writer closes before the next wait.");
    next_step(automatic);
    puts("\n--- STEP S2: wait again WITHOUT a new write ---");
    int ready = wait_ready(epfd);
    if (ready) {
        n = read_piece(fds[0]);
        if (n > 0) {
            total += n;
        }
    }

    int after_second = remaining(fds[0]);
    printf("S2 AFTER_SECOND second_ready=%d total_read=%d remaining=%d\n", ready, total,
           after_second);
    printf("RESULT mode=%s first_remaining=%d second_ready=%d second_remaining=%d\n", argv[1],
           after_first, ready, after_second);
    puts("Record S2 before Enter. Cleanup reads are a separate step.");
    next_step(automatic);
    puts("\n--- STEP S3: cleanup reads (outside the comparison) ---");
    puts("S3 CLEANUP: read the remaining bytes directly, without waiting for an event.");
    while (read_piece(fds[0]) > 0) {
    }

    close(epfd);
    close(fds[0]);
    close(fds[1]);
    printf("========== END: %s ==========\n\n", argv[1]);
    return 0;
}
