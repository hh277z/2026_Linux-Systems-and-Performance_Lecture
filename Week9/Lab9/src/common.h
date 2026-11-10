#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

static inline void fail(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}


static inline double now(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        fail("clock_gettime");
    }

    return value.tv_sec + value.tv_nsec / 1e9;
}


static inline long number(const char *text, long minimum, long maximum)
{
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno || *end || end == text || value < minimum || value > maximum) {
        fprintf(stderr, "Invalid integer: %s (%ld..%ld)\n", text, minimum, maximum);
        exit(2);
    }

    return value;
}


static inline void ready(void)
{
    char line[16];
    puts("READY");
    fflush(stdout);

    /* KEY: preparation finishes before the observer sends GO. */
    if (!fgets(line, sizeof line, stdin) || line[0] != 'G') {
        exit(2);
    }
}
