#ifndef LAB_COMMON_H
#define LAB_COMMON_H
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define MIB ((size_t)1024 * 1024)

static inline double now_seconds(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) { perror("clock_gettime"); exit(1); }
    return t.tv_sec + t.tv_nsec / 1e9;
}

static inline void join_group(const char *group)
{
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/cgroup.procs", group);
    if (n < 0 || (size_t)n >= sizeof path) { fputs("Group path too long\n", stderr); exit(1); }
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    if (fprintf(f, "%ld\n", (long)getpid()) < 0 || fclose(f)) { perror("join cgroup"); exit(1); }
}

static inline int stage(int number, const char *name, const char *next, int automatic)
{
    printf("\nSTAGE %d %s PID=%ld\nNEXT: %s\n", number, name, (long)getpid(), next);
    if (automatic) { usleep(700000); return 1; }
    puts("WAITING: inspect in terminal B, then press ENTER here.");
    int ch;
    do { ch = getchar(); } while (ch != '\n' && ch != EOF);
    return ch != EOF;
}

static inline void touch_pages(void *memory, size_t length)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    volatile unsigned char *p = memory;
    p[0] = 1;
    for (size_t i = page - (uintptr_t)memory % page; i < length; i += page) p[i] = 1;
    p[length - 1] = 1;
}
#endif
