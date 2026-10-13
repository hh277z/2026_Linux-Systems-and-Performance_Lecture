/* a5.h: helpers shared by Assignment 5 programs. Nothing here needs to change. */
#ifndef A5_H
#define A5_H
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif
#ifndef SCHED_FLAG_DL_OVERRUN
#define SCHED_FLAG_DL_OVERRUN 0x04
#endif

struct a5_sched_attr { /* struct sched_attr, include/uapi/linux/sched/types.h */
    uint32_t size, sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};

static inline int a5_sched_setattr(const struct a5_sched_attr *a) {
    return (int)syscall(SYS_sched_setattr, 0, a, 0);
}

static inline uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static inline uint64_t thread_cpu_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static inline long gettid_l(void) {
    return syscall(SYS_gettid);
}

/* Burn ns of this thread's CPU time. Preempted or throttled time does not count. */
static inline void burn_cpu(uint64_t ns) {
    volatile uint64_t s = 1;
    uint64_t end = thread_cpu_ns() + ns;
    while (thread_cpu_ns() < end) {
        for (int i = 0; i < 2048; i++) {
            s = s * UINT64_C(6364136223846793005) + 1;
        }
    }
}

static inline void sleep_until_ns(uint64_t t) {
    struct timespec ts = { .tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static inline int pin_self(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

static inline int set_fifo(int prio) {
    struct sched_param p = { .sched_priority = prio };
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* p in [0,100]; sorts v in place */
static inline uint64_t pct_u64(uint64_t *v, size_t n, int p) {
    if (!n) {
        return 0;
    }
    qsort(v, n, sizeof *v, cmp_u64);
    size_t i = (n * (size_t)p) / 100;
    return v[i >= n ? n - 1 : i];
}

#endif
