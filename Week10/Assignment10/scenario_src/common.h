#ifndef LAB10_COMMON_H
#define LAB10_COMMON_H
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/mman.h>

static volatile sig_atomic_t lab_stop;

static void lab_sig(int s) {
    (void)s;
    lab_stop = 1;
}

static double lab_now(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        perror("clock");
        exit(1);
    }

    return t.tv_sec + t.tv_nsec / 1e9;
}

static void lab_init(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction a = {0};
    a.sa_handler = lab_sig;
    sigaction(SIGTERM, &a, NULL);
    sigaction(SIGINT, &a, NULL);
}

static void lab_sleep(double s) {
    double end = lab_now() + s;
    while (!lab_stop && lab_now() < end) {
        struct timespec t = {0, 10000000};
        nanosleep(&t, NULL);
    }
}

static size_t lab_size(const char *s) {
    char *e;
    errno = 0;
    if (!s[0] || s[0] == '-')
        goto bad;

    unsigned long long n = strtoull(s, &e, 10), m = 1;

    if (errno || e == s)
        goto bad;

    if (*e) {
        if (!strcmp(e, "KiB"))
            m = 1024;
        else if (!strcmp(e, "MiB"))
            m = 1024 * 1024;
        else if (!strcmp(e, "GiB"))
            m = 1024ULL * 1024 * 1024;
        else
            goto bad;
    }

    if (n > SIZE_MAX / m)
        goto bad;

    return (size_t)(n * m);

bad:
    fprintf(stderr, "invalid size: %s\n", s);
    exit(2);
}

static double lab_number(const char *s, double lo, double hi) {
    char *e;
    errno = 0;
    double x = strtod(s, &e);

    if (errno || *e || e == s || !(x >= lo && x <= hi)) {
        fprintf(stderr, "invalid number: %s\n", s);
        exit(2);
    }

    return x;
}

static void lab_touch(void *v, size_t n) {
    volatile unsigned char *p = v;
    size_t step = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < n; i += step)
        p[i] = (unsigned char)(i / step + 1);

    if (n)
        p[n - 1] = 1;
}

static void lab_adj(int n) {
    FILE *f = fopen("/proc/self/oom_score_adj", "w");
    if (!f || fprintf(f, "%d", n) < 0 || fclose(f)) {
        perror("oom_score_adj");
        exit(1);
    }
}
#endif
