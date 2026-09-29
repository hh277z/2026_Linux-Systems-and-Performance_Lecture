#include "lab.h"
#include <sys/wait.h>

struct message {
    double sent_ms;
    char channel;
};

static unsigned long long empty_reads;
static char order[3];
static int received;

static bool receive_one(int fd, double start) {
    struct message m;
    ssize_t n;
    do {
        n = read(fd, &m, sizeof m); /* KEY_READ */
    } while (n < 0 && errno == EINTR);

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        ++empty_reads;
        return false;
    }

    if (n < 0) {
        fail("read");
    }

    if (n != (ssize_t)sizeof m) {
        fprintf(stderr, "Unexpected message size: %zd\n", n);
        exit(EXIT_FAILURE);
    }

    double at = now_ms();
    order[received++] = m.channel;
    printf("RECV channel=%c sent_ms=%.1f received_ms=%.1f delay_ms=%.1f\n", m.channel,
           m.sent_ms - start, at - start, at - m.sent_ms);
    return true;
}

static void send_one(int fd, char channel) {
    struct message m = {.sent_ms = now_ms(), .channel = channel};
    ssize_t n;
    do {
        n = write(fd, &m, sizeof m);
    } while (n < 0 && errno == EINTR);

    if (n != (ssize_t)sizeof m) {
        fprintf(stderr, "write failed\n");
        _exit(1);
    }
}

int main(int argc, char **argv) {
    if (argc != 2 ||
        (strcmp(argv[1], "block") && strcmp(argv[1], "busy") && strcmp(argv[1], "epoll"))) {
        fprintf(stderr, "Usage: %s block|busy|epoll\n", argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    bool blocking = !strcmp(argv[1], "block");
    bool multiplex = !strcmp(argv[1], "epoll");
    int data[2][2], gate[2];
    check(pipe(data[0]), "pipe A");
    check(pipe(data[1]), "pipe B");
    check(pipe(gate), "pipe gate");
    pid_t child = fork();
    check((int)child, "fork");
    if (!child) {
        close(gate[1]);
        close(data[0][0]);
        close(data[1][0]);
        char go;
        if (read(gate[0], &go, 1) != 1) {
            _exit(1);
        }

        close(gate[0]);
        sleep_ms(300);
        send_one(data[1][1], 'B');
        sleep_ms(1200);
        send_one(data[0][1], 'A');
        close(data[0][1]);
        close(data[1][1]);
        _exit(0);
    }

    close(gate[0]);
    close(data[0][1]);
    close(data[1][1]);
    int epfd = -1;
    for (int i = 0; i < 2; ++i) {
        if (!blocking) {
            /* Keep the reader non-blocking in busy and epoll modes. */
            nonblock(data[i][0]);
        }
    }

    if (multiplex) {
        epfd = epoll_create1(EPOLL_CLOEXEC);
        check(epfd, "epoll_create1");
        for (int i = 0; i < 2; ++i) {
            struct epoll_event ev = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
            check(epoll_ctl(epfd, EPOLL_CTL_ADD, data[i][0], &ev), "epoll_ctl");
        }
    }

    printf("\n========== WAIT MODE: %s ==========\n", argv[1]);
    printf("MODE=%s reader_pid=%ld writer_pid=%ld B~300ms A~1500ms\n", argv[1], (long)getpid(),
           (long)child);
    double cpu_start = cpu_ms(), start = now_ms();
    if (write(gate[1], "G", 1) != 1) {
        fail("start writer");
    }

    close(gate[1]);
    bool done[2] = {false, false};
    unsigned long long waits = 0;
    if (blocking) {
        /* Blocking mode waits for A before checking B. */
        receive_one(data[0][0], start); /* KEY_BLOCK_ORDER */
        receive_one(data[1][0], start);
    } else if (!multiplex) {
        while (received < 2) { /* KEY_BUSY */
            for (int i = 0; i < 2; ++i) {
                if (!done[i]) {
                    done[i] = receive_one(data[i][0], start);
                }
            }
        }
    } else {
        while (received < 2) {
            struct epoll_event events[2];
            /* Wait for a ready event from the registered FDs. */
            int n = epoll_wait(epfd, events, 2, 3000); /* KEY_WAIT */
            ++waits;
            if (n < 0 && errno == EINTR) {
                continue;
            }

            check(n, "epoll_wait");
            if (!n) {
                fprintf(stderr, "Writer timeout\n");
                return 1;
            }

            for (int j = 0; j < n; ++j) {
                /* Recover the channel index saved during registration. */
                int i = (int)events[j].data.u32;
                if (!done[i] && receive_one(data[i][0], start)) {
                    done[i] = true;
                    check(epoll_ctl(epfd, EPOLL_CTL_DEL, data[i][0], NULL), "DEL");
                }
            }
        }
    }

    double elapsed = now_ms() - start, used = cpu_ms() - cpu_start;
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            fail("waitpid");
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        return 1;
    }

    for (int i = 0; i < 2; ++i) {
        close(data[i][0]);
    }

    if (epfd >= 0) {
        close(epfd);
    }

    puts("\n--- Measurement result ---");
    printf("RESULT mode=%s order=%s elapsed_ms=%.1f cpu_ms=%.1f empty_reads=%llu waits=%llu\n",
           argv[1], order, elapsed, used, empty_reads, waits);
    printf("========== END: %s ==========\n\n", argv[1]);
    return 0;
}

