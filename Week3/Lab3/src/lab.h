#ifndef WEEK3_LAB_H
#define WEEK3_LAB_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static inline void fail(const char *what) {
    perror(what);
    exit(EXIT_FAILURE);
}

static inline void check(int rc, const char *what) {
    if (rc < 0) {
        fail(what);
    }
}

static inline double now_ms(void) {
    struct timespec ts;
    check(clock_gettime(CLOCK_MONOTONIC, &ts), "clock_gettime");
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static inline void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) < 0) {
        if (errno != EINTR) {
            fail("nanosleep");
        }
    }
}

static inline double cpu_ms(void) {
    struct rusage r;
    check(getrusage(RUSAGE_SELF, &r), "getrusage");
    return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0 +
           (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0;
}

static inline void nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL);
    check(flags, "F_GETFL");
    check(fcntl(fd, F_SETFL, flags | O_NONBLOCK), "F_SETFL");
}

static inline void next_step(bool automatic) {
    if (automatic) {
        return;
    }

    char line[128];
    printf("Enter -> next step: ");
    fflush(stdout);
    if (!fgets(line, sizeof line, stdin)) {
        fprintf(stderr, "Input ended; closing this demo.\n");
        exit(EXIT_FAILURE);
    }
}

#endif
