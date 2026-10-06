/* mtwork: multi-threaded workload for scheduling experiments.
 *
 * usage: mtwork [-d seconds] [-n NAME] [-c PARENT] <spec>...
 *   -d seconds   run time (default 5)
 *   -n NAME      thread name prefix (default mtwork): threads become NAME-0, NAME-1, ... (max 13 chars)
 *   -c PARENT    put this process into /sys/fs/cgroup/PARENT (e.g. lab/bg) and allow per-thread cg=NAME
 *   spec         one thread: WORK[,cpu=N][,sched=POLICY][,cg=NAME]
 *     WORK   = busy | periodic[:period_us[:work_us]] | sysio   (periodic default 10000:2000; sysio = 1-byte writes to /dev/null)
 *     POLICY = other | fifo:PRIO | rr:PRIO | dl:RUNTIME_US/PERIOD_US   (rt/dl need root)
 *     cg=NAME  move this thread into /sys/fs/cgroup/PARENT/NAME/cgroup.threads (needs -c, root)
 *
 * example: sudo ./bin/mtwork -d 5 busy,cpu=0,sched=fifo:60 busy,cpu=0,sched=rr:50 busy,cpu=0 periodic,sched=dl:2000/10000
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#define MAX_THREADS 32
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif

struct dl_attr { /* mirrors struct sched_attr */
    uint32_t size, policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime, deadline, period;
    uint32_t util_min, util_max;
};

struct spec {
    int periodic;            /* 0 busy, 1 periodic, 2 sysio */
    uint64_t period_ns, work_ns;
    int cpu;                 /* -1 none */
    char policy[8];          /* other fifo rr dl */
    int prio;
    uint64_t dl_runtime_ns, dl_period_ns;
    char cg[32];
};

struct result {
    long tid;
    int last_cpu;
    double cpu_ms;
    uint64_t exec_ns, wait_ns, pcount;
    long vol, invol;
    uint64_t rounds, iters, late_p99_ns, late_max_ns, misses;
    int failed;
};

static char cg_parent[64];
static char name_prefix[16] = "mtwork";
static uint64_t run_ns;
static pthread_barrier_t barrier;
static struct spec specs[MAX_THREADS];
static struct result results[MAX_THREADS];

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int devnull = -1;

/* The unit of work. Kept out of line so that uprobes (Week 5) can attach to it. */
__attribute__((noinline)) uint64_t work_item(uint64_t ns) {
    uint64_t state = 1, rounds = 0, end = now_ns() + ns;
    do {
        for (int i = 0; i < 4096; i++) {
            state = state * UINT64_C(6364136223846793005) + 1;
        }
        rounds++;
    } while (now_ns() < end);
    return rounds + (state == 0);
}

/* One syscall-heavy unit: n one-byte writes to /dev/null. uprobe target for Week 5. */
__attribute__((noinline)) uint64_t sysio_item(int n) {
    uint64_t done = 0;
    for (int i = 0; i < n; i++) {
        if (write(devnull, "x", 1) == 1) {
            done++;
        }
    }
    return done;
}

/* Sleep until an absolute deadline. Out of line so that its latency can be probed. */
__attribute__((noinline)) void wait_item(const struct timespec *deadline) {
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL) == EINTR) {
    }
}

static int parse_spec(char *s, struct spec *sp) {
    char *save = NULL, *tok = strtok_r(s, ",", &save);
    memset(sp, 0, sizeof *sp);
    sp->cpu = -1;
    strcpy(sp->policy, "other");
    if (!tok) {
        return -1;
    }
    if (!strcmp(tok, "busy")) {
        sp->periodic = 0;
    } else if (!strcmp(tok, "sysio")) {
        sp->periodic = 2;
    } else if (!strncmp(tok, "periodic", 8)) {
        long p = 10000, w = 2000;
        sp->periodic = 1;
        if (tok[8] == ':' && sscanf(tok + 9, "%ld:%ld", &p, &w) < 1) {
            return -1;
        }
        sp->period_ns = (uint64_t)p * 1000;
        sp->work_ns = (uint64_t)w * 1000;
    } else {
        return -1;
    }
    while ((tok = strtok_r(NULL, ",", &save))) {
        if (!strncmp(tok, "cpu=", 4)) {
            sp->cpu = atoi(tok + 4);
        } else if (!strncmp(tok, "sched=", 6)) {
            const char *p = tok + 6;
            if (!strcmp(p, "other")) {
                strcpy(sp->policy, "other");
            } else if (!strncmp(p, "fifo:", 5) || !strncmp(p, "rr:", 3)) {
                strcpy(sp->policy, p[0] == 'f' ? "fifo" : "rr");
                sp->prio = atoi(strchr(p, ':') + 1);
                if (sp->prio < 1 || sp->prio > 99) {
                    return -1;
                }
            } else if (!strncmp(p, "dl:", 3)) {
                long r, d;
                if (sscanf(p + 3, "%ld/%ld", &r, &d) != 2 || r <= 0 || d < r) {
                    return -1;
                }
                strcpy(sp->policy, "dl");
                sp->dl_runtime_ns = (uint64_t)r * 1000;
                sp->dl_period_ns = (uint64_t)d * 1000;
            } else {
                return -1;
            }
        } else if (!strncmp(tok, "cg=", 3)) {
            strncpy(sp->cg, tok + 3, sizeof sp->cg - 1);
        } else {
            return -1;
        }
    }
    if (!strcmp(sp->policy, "dl") && sp->cpu >= 0) {
        fprintf(stderr, "dl threads cannot be pinned (cpu=N with sched=dl)\n");
        return -1;
    }
    if (sp->cg[0] && !cg_parent[0]) {
        fprintf(stderr, "cg=NAME needs -c PARENT\n");
        return -1;
    }
    return 0;
}

static int write_str(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (!f) {
        return -1;
    }
    int rc = fprintf(f, "%s\n", s) < 0 ? -1 : 0;
    return fclose(f) ? -1 : rc;
}

static int apply_policy(const struct spec *sp) {
    struct sched_param p = { .sched_priority = sp->prio };
    if (!strcmp(sp->policy, "fifo")) {
        return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p); /* KEY per-thread rt policy */
    }
    if (!strcmp(sp->policy, "rr")) {
        return pthread_setschedparam(pthread_self(), SCHED_RR, &p);
    }
    if (!strcmp(sp->policy, "dl")) {
        struct dl_attr a = { .size = sizeof a, .policy = SCHED_DEADLINE, .runtime = sp->dl_runtime_ns,
                             .deadline = sp->dl_period_ns, .period = sp->dl_period_ns };
        return (int)syscall(SYS_sched_setattr, 0, &a, 0); /* KEY deadline reservation for this thread */
    }
    return 0;
}

static int read_schedstat(long tid, uint64_t *exec, uint64_t *wait, uint64_t *count) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%ld/schedstat", tid);
    FILE *f = fopen(path, "r");
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

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void *thread_main(void *arg) {
    int idx = (int)(intptr_t)arg;
    struct spec *sp = &specs[idx];
    struct result *r = &results[idx];
    char name[16];
    r->tid = syscall(SYS_gettid);
    snprintf(name, sizeof name, "%.13s-%d", name_prefix, idx);
    pthread_setname_np(pthread_self(), name); /* KEY comm shows up per thread in perf and ps -L */
    if (sp->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(sp->cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set)) { /* KEY this thread only */
            perror("pthread_setaffinity_np");
            r->failed = 1;
        }
    }
    if (sp->cg[0]) {
        char path[128], tidstr[16];
        snprintf(path, sizeof path, "/sys/fs/cgroup/%s/%s/cgroup.threads", cg_parent, sp->cg);
        snprintf(tidstr, sizeof tidstr, "%ld", r->tid);
        if (write_str(path, tidstr)) { /* KEY threaded cgroup: one thread into a child group */
            fprintf(stderr, "thread %d: cannot write %s: %s\n", idx, path, strerror(errno));
            r->failed = 1;
        }
    }
    if (apply_policy(sp)) {
        fprintf(stderr, "thread %d: sched=%s: %s%s\n", idx, sp->policy, strerror(errno),
                errno == EPERM ? " (root needed; dl must be unpinned)" : errno == EBUSY ? " (dl admission control)" : "");
        r->failed = 1;
    }
    pthread_barrier_wait(&barrier); /* KEY all threads start the measured interval together */
    uint64_t e0, w0, c0, e1, w1, c1;
    struct rusage u0, u1;
    read_schedstat(r->tid, &e0, &w0, &c0);
    getrusage(RUSAGE_THREAD, &u0);
    uint64_t t0 = now_ns(), end = t0 + run_ns;
    if (sp->periodic == 0) {
        while (now_ns() < end) {
            r->rounds += work_item(1000000); /* 1 ms chunks so the unit of work is visible to a profiler */
        }
    } else if (sp->periodic == 2) {
        while (now_ns() < end) {
            r->rounds += sysio_item(1000); /* KEY syscall-bound: each write enters the kernel */
        }
    } else {
        uint64_t cap = run_ns / sp->period_ns + 2, *late = calloc(cap, sizeof *late), n = 0;
        struct timespec next = { .tv_sec = (time_t)(t0 / 1000000000ull), .tv_nsec = (long)(t0 % 1000000000ull) };
        for (uint64_t i = 0; now_ns() < end && n < cap; i++) {
            uint64_t expected = t0 + i * sp->period_ns, start = now_ns();
            late[n++] = start > expected ? start - expected : 0;
            r->rounds += work_item(sp->work_ns);
            if (now_ns() > expected + sp->period_ns) {
                r->misses++; /* KEY deadline miss: the frame finished after the next period began */
            }
            if (!strcmp(sp->policy, "dl")) {
                sched_yield(); /* KEY dl: give back the rest of the period */
            } else {
                next.tv_nsec += (long)(sp->period_ns % 1000000000ull);
                next.tv_sec += (time_t)(sp->period_ns / 1000000000ull);
                if (next.tv_nsec >= 1000000000L) {
                    next.tv_nsec -= 1000000000L;
                    next.tv_sec++;
                }
                wait_item(&next); /* KEY off-CPU part of the period */
            }
        }
        qsort(late, n, sizeof *late, cmp_u64);
        r->iters = n;
        r->late_p99_ns = n ? late[n * 99 / 100] : 0;
        r->late_max_ns = n ? late[n - 1] : 0;
        free(late);
    }
    getrusage(RUSAGE_THREAD, &u1);
    read_schedstat(r->tid, &e1, &w1, &c1);
    r->last_cpu = sched_getcpu();
    r->cpu_ms = (u1.ru_utime.tv_sec - u0.ru_utime.tv_sec) * 1e3 + (u1.ru_utime.tv_usec - u0.ru_utime.tv_usec) / 1e3 +
                (u1.ru_stime.tv_sec - u0.ru_stime.tv_sec) * 1e3 + (u1.ru_stime.tv_usec - u0.ru_stime.tv_usec) / 1e3;
    r->exec_ns = e1 - e0;
    r->wait_ns = w1 - w0;
    r->pcount = c1 - c0;
    r->vol = u1.ru_nvcsw - u0.ru_nvcsw;
    r->invol = u1.ru_nivcsw - u0.ru_nivcsw;
    return NULL;
}

int main(int argc, char **argv) {
    double seconds = 5;
    int opt;
    while ((opt = getopt(argc, argv, "d:n:c:h")) != -1) {
        if (opt == 'd') {
            seconds = atof(optarg);
        } else if (opt == 'n') {
            strncpy(name_prefix, optarg, sizeof name_prefix - 1);
        } else if (opt == 'c') {
            strncpy(cg_parent, optarg, sizeof cg_parent - 1);
        } else {
            fprintf(stderr, "usage: %s [-d seconds] [-n NAME] [-c PARENT] <spec>...\n  spec: busy|periodic[:period_us[:work_us]]|sysio[,cpu=N][,sched=other|fifo:P|rr:P][,cg=NAME]\n", argv[0]);
            return 1;
        }
    }
    int n = argc - optind;
    if (n < 1 || n > MAX_THREADS) {
        fprintf(stderr, "give 1..%d thread specs\n", MAX_THREADS);
        return 1;
    }
    run_ns = (uint64_t)(seconds * 1e9);
    devnull = open("/dev/null", O_WRONLY);
    char *raw[MAX_THREADS];
    for (int i = 0; i < n; i++) {
        raw[i] = strdup(argv[optind + i]);
        if (parse_spec(argv[optind + i], &specs[i])) {
            fprintf(stderr, "bad spec: %s\n", raw[i]);
            return 1;
        }
    }
    if (cg_parent[0]) {
        char path[128], pid[16];
        snprintf(path, sizeof path, "/sys/fs/cgroup/%s/cgroup.procs", cg_parent);
        snprintf(pid, sizeof pid, "%d", (int)getpid());
        if (write_str(path, pid)) { /* KEY the process goes to the threaded domain root first */
            fprintf(stderr, "cannot join cgroup %s: %s (run tools/tcgroup.sh setup first, as root)\n", path, strerror(errno));
            return 1;
        }
    }
    pthread_barrier_init(&barrier, NULL, (unsigned)n + 1);
    pthread_t th[MAX_THREADS];
    for (int i = 0; i < n; i++) {
        if (pthread_create(&th[i], NULL, thread_main, (void *)(intptr_t)i)) {
            perror("pthread_create");
            return 1;
        }
    }
    pthread_barrier_wait(&barrier);
    /* threads have set affinity/policy/cgroup; publish ids for external tools */
    printf("pid=%d name=%s duration=%.1fs threads=%d", (int)getpid(), name_prefix, seconds, n);
    if (cg_parent[0]) {
        printf(" cgroup_parent=%s", cg_parent);
    }
    printf("\nTIDS=");
    for (int i = 0; i < n; i++) {
        printf("%s%ld", i ? "," : "", results[i].tid);
    }
    printf("\n");
    fflush(stdout);
    for (int i = 0; i < n; i++) {
        pthread_join(th[i], NULL);
    }
    printf("%-3s %-30s %-6s %7s %4s %8s %8s %8s %6s %5s %6s %10s %8s %9s %6s\n", "idx", "spec", "cg", "tid", "cpu", "cpu_ms",
           "exec_ms", "wait_ms", "pcount", "vol", "invol", "rounds", "iters", "late_p99u", "miss");
    double total = 0;
    for (int i = 0; i < n; i++) {
        struct result *r = &results[i];
        printf("%-3d %-30.30s %-6s %7ld %4d %8.1f %8.1f %8.1f %6llu %5ld %6ld %10llu %8llu %9.1f %6llu%s\n", i, raw[i],
               specs[i].cg[0] ? specs[i].cg : "-", r->tid, r->last_cpu, r->cpu_ms, r->exec_ns / 1e6, r->wait_ns / 1e6,
               (unsigned long long)r->pcount, r->vol, r->invol, (unsigned long long)r->rounds, (unsigned long long)r->iters,
               r->late_p99_ns / 1e3, (unsigned long long)r->misses, r->failed ? "  (setup failed)" : "");
        total += r->cpu_ms;
    }
    printf("total cpu_ms=%.1f  (%.2f CPUs over %.1fs)\n", total, total / (seconds * 1000.0), seconds);
    return 0;
}
