/*
 * common.h - small helpers shared by bqtest and albumsvc.
 *
 * Nothing in this file needs to change for the assignment.
 */
#ifndef COMMON_H
#define COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* needed for pthread_setname_np and CPU_SET (.c files define it first) */
#endif
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAYBE_UNUSED __attribute__((unused))   /* no warning while the TODO that uses it is a stub */

/* ---- time ------------------------------------------------------------ */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Sleep until an absolute CLOCK_MONOTONIC time (ns). */
static inline void sleep_until_ns(uint64_t t)
{
    struct timespec ts = { .tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0)
        ;
}

static inline void sleep_us(uint64_t us)
{
    sleep_until_ns(now_ns() + us * 1000ull);
}

/* Burn `us` microseconds of this thread's CPU time (not wall time). */
static inline void burn_cpu_us(uint64_t us)
{
    uint64_t end = thread_cpu_ns() + us * 1000ull;
    while (thread_cpu_ns() < end)
        ;
}

/* ---- threads --------------------------------------------------------- */

static inline void set_thread_name(const char *name)
{
    pthread_setname_np(pthread_self(), name);
}

static inline int pin_to_cpu(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

/* ---- statistics ------------------------------------------------------ */

static inline int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Percentile p (0..100) of v[0..n-1]. Sorts v in place. Returns 0 for n == 0. */
static inline uint64_t percentile(uint64_t *v, size_t n, double p)
{
    if (n == 0)
        return 0;
    qsort(v, n, sizeof v[0], cmp_u64);
    size_t i = (size_t)(p / 100.0 * (double)(n - 1) + 0.5);
    return v[i];
}

#endif /* COMMON_H */
