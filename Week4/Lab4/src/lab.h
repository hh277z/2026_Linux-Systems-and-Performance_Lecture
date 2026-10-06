#ifndef LAB_H
#define LAB_H
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline void die(const char *what) {
    perror(what);
    exit(1);
}

static inline uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) {
        die("clock_gettime");
    }
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* Restrict the calling task to one CPU so that competitors share a runqueue. */
static inline void pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set)) {
        die("sched_setaffinity");
    }
}

/* /proc/self/schedstat: sum_exec_runtime(ns) run_delay(ns) pcount */
static inline int read_schedstat(uint64_t *exec, uint64_t *wait, uint64_t *count) {
    FILE *f = fopen("/proc/self/schedstat", "r"); /* KEY observation path */
    if (!f) {
        return -1;
    }
    unsigned long long a = 0, b = 0, c = 0;
    int n = fscanf(f, "%llu %llu %llu", &a, &b, &c);
    fclose(f);
    *exec = a;
    *wait = b;
    *count = c;
    return n == 3 ? 0 : -1;
}

/* CPU-bound loop for ns nanoseconds of wall time. Returns loop rounds. */
static inline uint64_t busy_for(uint64_t ns) {
    uint64_t state = 1, rounds = 0, end = now_ns() + ns;
    do {
        for (int i = 0; i < 4096; i++) {
            state = state * UINT64_C(6364136223846793005) + 1;
        }
        rounds++;
    } while (now_ns() < end);
    return rounds + (state == 0);
}

static inline double ms(uint64_t ns) {
    return (double)ns / 1e6;
}

#endif

/* ---- Week 5 additions ---- */
#include <pthread.h>
#include <stdatomic.h>

/* CPU time of the calling thread in ms (user + system). */
static inline double thread_cpu_ms(void) {
    struct rusage u;
    if (getrusage(RUSAGE_THREAD, &u)) {
        die("getrusage");
    }
    return u.ru_utime.tv_sec * 1000.0 + u.ru_utime.tv_usec / 1000.0 +
           u.ru_stime.tv_sec * 1000.0 + u.ru_stime.tv_usec / 1000.0;
}

static inline int online_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
