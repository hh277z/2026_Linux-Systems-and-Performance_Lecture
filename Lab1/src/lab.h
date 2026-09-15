#ifndef WEEK1_LAB_H
#define WEEK1_LAB_H
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static inline void die(const char *what) {
    perror(what);
    exit(EXIT_FAILURE);
}

static inline long tid(void) {
    return syscall(SYS_gettid);
}

static inline double seconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        die("clock_gettime");
    }
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static inline double cpu_ms(struct timeval t) {
    return 1000.0 * (double)t.tv_sec + (double)t.tv_usec / 1000.0;
}

static inline long number(const char *s, long lo, long hi) {
    char *end = NULL;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || !*s || *end || n < lo || n > hi) {
        fprintf(stderr, "Expected integer in [%ld, %ld]: %s\n", lo, hi, s);
        exit(EXIT_FAILURE);
    }
    return n;
}

static inline void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&t, &t) < 0) {
        if (errno != EINTR) {
            die("nanosleep");
        }
    }
}

static inline void checkpoint(int automatic, const char *message) {
    printf("\n%s\n", message);

    if (!automatic) {
        printf("Press Enter to continue (EOF also continues): ");
        fflush(stdout);
        int ch;
        while ((ch = getchar()) != '\n' && ch != EOF) {
        }
    }

    printf("\n------------------------------------------------------------\n\n");
    fflush(stdout);
}
#endif
