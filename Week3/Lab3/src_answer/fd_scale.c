#include "lab.h"
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/select.h>

/* Linux select accepts a bitmap sized for nfds. FD_SET/FD_ISSET must not be
 * used beyond the fixed-size glibc fd_set (FD_SETSIZE, normally 1024). */
static size_t bitmap_words(int nfds) {
    return ((size_t)nfds + sizeof(unsigned long) * 8 - 1) / (sizeof(unsigned long) * 8);
}

static void bit_set(unsigned long *bits, int fd) {
    size_t width = sizeof(unsigned long) * 8;
    bits[(size_t)fd / width] |= 1UL << ((size_t)fd % width);
}

static bool bit_test(const unsigned long *bits, int fd) {
    size_t width = sizeof(unsigned long) * 8;
    return (bits[(size_t)fd / width] & (1UL << ((size_t)fd % width))) != 0;
}

static void require_fd_limit(int count) {
    struct rlimit limit;
    check(getrlimit(RLIMIT_NOFILE, &limit), "getrlimit");
    if (limit.rlim_cur < (rlim_t)count + 16) {
        fprintf(stderr, "Need at least %d open files; current soft limit is %llu\n", count + 16,
                (unsigned long long)limit.rlim_cur);
        exit(2);
    }
}

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "select") && strcmp(argv[1], "poll") &&
                      strcmp(argv[1], "epoll"))) {
        fprintf(stderr, "Usage: %s select|poll|epoll FD_COUNT ROUNDS\n", argv[0]);
        return 2;
    }

    char *end;
    long count_long = strtol(argv[2], &end, 10);
    if (*end || count_long < 3 || count_long > 50000) {
        fprintf(stderr, "FD_COUNT must be 3..50000\n");
        return 2;
    }

    long rounds_long = strtol(argv[3], &end, 10);
    if (*end || rounds_long < 1 || rounds_long > 10000) {
        fprintf(stderr, "ROUNDS must be 1..10000\n");
        return 2;
    }

    int count = (int)count_long, rounds = (int)rounds_long;
    require_fd_limit(count);
    int *fds = calloc((size_t)count, sizeof *fds);
    struct pollfd *pollfds = calloc((size_t)count, sizeof *pollfds);
    if (!fds || !pollfds) {
        fail("calloc");
    }

    double setup_start = now_ms();
    int maxfd = -1;
    for (int i = 0; i < count; ++i) {
        fds[i] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        check(fds[i], "eventfd");
        if (fds[i] > maxfd) {
            maxfd = fds[i];
        }

        pollfds[i] = (struct pollfd){.fd = fds[i], .events = POLLIN};
    }

    int epfd = -1;
    if (!strcmp(argv[1], "epoll")) {
        epfd = epoll_create1(EPOLL_CLOEXEC);
        check(epfd, "epoll_create1");
        for (int i = 0; i < count; ++i) {
            struct epoll_event event = {.events = EPOLLIN, .data.u32 = (uint32_t)i};
            check(epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i], &event), "epoll_ctl");
        }
    }

    uint64_t one = 1;
    for (int i = 0; i < 3; ++i) {
        if (write(fds[i], &one, sizeof one) != (ssize_t)sizeof one) {
            fail("eventfd write");
        }
    }

    double setup_ms = now_ms() - setup_start;
    size_t words = bitmap_words(maxfd + 1);
    unsigned long *bits = calloc(words, sizeof *bits);
    if (!bits) {
        fail("bitmap calloc");
    }

    printf("SETUP mode=%s watched=%d ready=3 maxfd=%d rounds=%d setup_ms=%.3f\n", argv[1],
           count, maxfd, rounds, setup_ms);
    double cpu_start = cpu_ms(), wall_start = now_ms();
    long long returned = 0, matched = 0, user_scanned = 0;
    for (int round = 0; round < rounds; ++round) {
        int n;
        if (!strcmp(argv[1], "select")) {
            memset(bits, 0, words * sizeof *bits);
            for (int i = 0; i < count; ++i) {
                bit_set(bits, fds[i]);
            }

            struct timeval zero = {0, 0};
            /* select uses the highest FD plus one as nfds. */
            n = select(maxfd + 1, (fd_set *)bits, NULL, NULL, &zero); /* KEY_SELECT */
            check(n, "select");
            for (int i = 0; i < count; ++i) {
                matched += bit_test(bits, fds[i]);
            }

            user_scanned += count;
        } else if (!strcmp(argv[1], "poll")) {
            /* poll receives the complete pollfd array on every wait. */
            n = poll(pollfds, (nfds_t)count, 0); /* KEY_POLL */
            check(n, "poll");
            for (int i = 0; i < count; ++i) {
                matched += (pollfds[i].revents & POLLIN) != 0;
            }

            user_scanned += count;
        } else {
            struct epoll_event ready[3];
            /* epoll_wait uses the registered epoll instance. */
            n = epoll_wait(epfd, ready, 3, 0); /* KEY_EPOLL */
            check(n, "epoll_wait");
            for (int i = 0; i < n; ++i) {
                matched += (ready[i].events & EPOLLIN) != 0;
            }

            user_scanned += n;
        }

        if (n != 3) {
            fprintf(stderr, "Expected 3 ready FDs, got %d in round %d\n", n, round);
            return 1;
        }

        returned += n;
    }

    double wall_ms = now_ms() - wall_start, used_ms = cpu_ms() - cpu_start;
    printf("RESULT mode=%s watched=%d ready=3 rounds=%d returned=%lld matched=%lld "
           "user_scanned=%lld "
           "wall_ms=%.3f cpu_ms=%.3f us_per_wait=%.2f\n",
           argv[1], count, rounds, returned, matched, user_scanned, wall_ms, used_ms,
           wall_ms * 1000.0 / rounds);
    free(bits);
    for (int i = 0; i < count; ++i) {
        close(fds[i]);
    }

    if (epfd >= 0) {
        close(epfd);
    }

    free(pollfds);
    free(fds);
    return 0;
}

