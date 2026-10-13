/* mtwork: multi-threaded workload for scheduling-class experiments (Week 5 version).
 *
 * usage: mtwork [-d seconds] [-n NAME] <spec>...
 *   -d seconds   run time (default 5)
 *   -n NAME      thread name prefix (default mtwork): threads become NAME-0, NAME-1, ... (max 13 chars)
 *   spec         one thread: WORK[,cpu=N][,sched=POLICY][,nice=N]
 *     WORK   = busy | periodic[:period_us[:work_us]]          (periodic default 10000:2000)
 *              periodic work is measured in CPU time of the thread: a 4000 us job needs 4 ms on the CPU,
 *              no matter how often it is preempted or throttled
 *     POLICY = other | batch | idle | fifo:PRIO | rr:PRIO | dl:RUNTIME_US/PERIOD_US   (fifo/rr/dl need root)
 *     nice=N   nice value of this thread only (other/batch), -20..19 (negative needs root)
 *
 * example: sudo ./bin/mtwork -d 5 busy,cpu=0,sched=fifo:50 busy,cpu=0,sched=rr:50 busy,cpu=0
 *          sudo ./bin/mtwork -d 5 -n dl periodic:10000:1500,sched=dl:2000/10000
 */
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
#include <sys/resource.h>
#include <sys/syscall.h>

#define MAX_THREADS 32
#ifndef SCHED_BATCH
#define SCHED_BATCH 3
#endif
#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif

struct dl_attr { /* mirrors struct sched_attr (include/uapi/linux/sched/types.h) */
    uint32_t size, policy;
    uint64_t flags;
    int32_t nice;
    uint32_t priority;
    uint64_t runtime, deadline, period;
    uint32_t util_min, util_max;
};

struct spec {
    int periodic;            /* 0 busy, 1 periodic */
    uint64_t period_ns, work_ns;
    int cpu;                 /* -1 none */
    char policy[8];          /* other batch idle fifo rr dl */
    int prio;
    int nice, has_nice;
    uint64_t dl_runtime_ns, dl_period_ns;
};

struct result {
    long tid;
    int last_cpu;
    double cpu_ms;
    uint64_t exec_ns, wait_ns;
    long vol, invol;
    uint64_t rounds, iters, late_p99_ns, resp_p99_ns, resp_max_ns, misses;
    int failed;
};

static char name_prefix[16] = "mtwork";
static uint64_t run_ns, start_ns; /* start_ns: one common start for every thread */
static pthread_barrier_t barrier;
static struct spec specs[MAX_THREADS];
static struct result results[MAX_THREADS];

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t thread_cpu_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static uint64_t spin(void) {
    uint64_t state = 1;
    for (int i = 0; i < 4096; i++) {
        state = state * UINT64_C(6364136223846793005) + 1;
    }
    return state == 0;
}

/* busy thread: 1 ms of wall time per call. Kept out of line so that a profiler sees the unit of work. */
__attribute__((noinline)) uint64_t work_item(uint64_t ns) {
    uint64_t rounds = 0, end = now_ns() + ns;
    do {
        rounds += 1 + spin();
    } while (now_ns() < end);
    return rounds;
}

/* periodic job: ns of this thread's own CPU time. Time spent preempted or throttled does not count. */
__attribute__((noinline)) uint64_t work_cpu(uint64_t ns) {
    uint64_t rounds = 0, end = thread_cpu_ns() + ns; /* KEY job size in CPU time, not wall time */
    do {
        rounds += 1 + spin();
    } while (thread_cpu_ns() < end);
    return rounds;
}

/* uprobe anchors: one call at the start and end of every periodic job (Assignment 5 attaches here). */
__attribute__((noinline)) void job_begin(uint64_t id) { __asm__ volatile("" : : "r"(id) : "memory"); }
__attribute__((noinline)) void job_end(uint64_t id) { __asm__ volatile("" : : "r"(id) : "memory"); }

/* Sleep until an absolute time. */
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
    } else if (!strncmp(tok, "periodic", 8)) {
        long p = 10000, w = 2000;
        sp->periodic = 1;
        if (tok[8] == ':' && sscanf(tok + 9, "%ld:%ld", &p, &w) < 1) {
            return -1;
        }
        if (p <= 0 || w <= 0) {
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
        } else if (!strncmp(tok, "nice=", 5)) {
            sp->nice = atoi(tok + 5);
            sp->has_nice = 1;
        } else if (!strncmp(tok, "sched=", 6)) {
            const char *p = tok + 6;
            if (!strcmp(p, "other") || !strcmp(p, "batch") || !strcmp(p, "idle")) {
                strcpy(sp->policy, p);
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
        } else {
            return -1;
        }
    }
    if (!strcmp(sp->policy, "dl") && sp->cpu >= 0) {
        fprintf(stderr, "dl threads cannot be pinned (cpu=N with sched=dl)\n");
        return -1;
    }
    return 0;
}

static int apply_policy(const struct spec *sp, long tid) {
    struct sched_param p = { .sched_priority = sp->prio };
    if (!strcmp(sp->policy, "fifo") || !strcmp(sp->policy, "rr")) {
        int pol = sp->policy[0] == 'f' ? SCHED_FIFO : SCHED_RR;
        int rc = pthread_setschedparam(pthread_self(), pol, &p); /* KEY rt: fixed priority 1..99 */
        if (rc) {
            errno = rc;
            return -1;
        }
        return 0;
    }
    if (!strcmp(sp->policy, "batch") || !strcmp(sp->policy, "idle")) {
        p.sched_priority = 0;
        int pol = sp->policy[0] == 'b' ? SCHED_BATCH : SCHED_IDLE;
        int rc = pthread_setschedparam(pthread_self(), pol, &p); /* KEY fair class, different wake-up rule */
        if (rc) {
            errno = rc;
            return -1;
        }
    }
    if (!strcmp(sp->policy, "dl")) {
        struct dl_attr a = { .size = sizeof a, .policy = SCHED_DEADLINE, .runtime = sp->dl_runtime_ns,
                             .deadline = sp->dl_period_ns, .period = sp->dl_period_ns };
        return (int)syscall(SYS_sched_setattr, 0, &a, 0); /* KEY CBS reservation: runtime per period */
    }
    if (sp->has_nice && setpriority(PRIO_PROCESS, (id_t)tid, sp->nice)) { /* KEY nice of this thread (TID) only */
        return -1;
    }
    return 0;
}

static int read_schedstat(long tid, uint64_t *exec, uint64_t *wait) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%ld/schedstat", tid);
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    unsigned long long a = 0, b = 0;
    int n = fscanf(f, "%llu %llu", &a, &b);
    fclose(f);
    *exec = a;
    *wait = b;
    return n == 2 ? 0 : -1;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void add_ns(struct timespec *t, uint64_t ns) {
    t->tv_sec += (time_t)(ns / 1000000000ull);
    t->tv_nsec += (long)(ns % 1000000000ull);
    if (t->tv_nsec >= 1000000000L) {
        t->tv_nsec -= 1000000000L;
        t->tv_sec++;
    }
}

static void run_periodic(struct spec *sp, struct result *r, uint64_t t0, uint64_t end) {
    int dl = !strcmp(sp->policy, "dl");
    uint64_t cap = run_ns / sp->period_ns + 2, n = 0;
    uint64_t *late = calloc(cap, sizeof *late), *resp = calloc(cap, sizeof *resp);
    struct timespec next = { .tv_sec = (time_t)(t0 / 1000000000ull), .tv_nsec = (long)(t0 % 1000000000ull) };
    for (uint64_t i = 0; now_ns() < end && n < cap; i++) {
        uint64_t start = now_ns();
        /* release time of job i: a fixed grid for sleep-based threads. A deadline thread is released by
         * the kernel at each new period, so its job starts when it is released and we measure from there. */
        uint64_t release = dl ? start : t0 + i * sp->period_ns;
        late[n] = start > release ? start - release : 0;
        job_begin(i);
        r->rounds += work_cpu(sp->work_ns);
        job_end(i);
        uint64_t done = now_ns();
        resp[n] = done - release;
        if (done > release + sp->period_ns) {
            r->misses++; /* KEY deadline miss: the job finished after its deadline (= end of its period) */
        }
        n++;
        if (dl) {
            sched_yield(); /* KEY dl: job done, sleep until the next period (budget refilled then) */
        } else {
            add_ns(&next, sp->period_ns);
            wait_item(&next); /* off-CPU part of the period */
        }
    }
    qsort(late, n, sizeof *late, cmp_u64);
    qsort(resp, n, sizeof *resp, cmp_u64);
    r->iters = n;
    r->late_p99_ns = n ? late[n * 99 / 100] : 0;
    r->resp_p99_ns = n ? resp[n * 99 / 100] : 0;
    r->resp_max_ns = n ? resp[n - 1] : 0;
    free(late);
    free(resp);
}

static void *thread_main(void *arg) {
    int idx = (int)(intptr_t)arg;
    struct spec *sp = &specs[idx];
    struct result *r = &results[idx];
    char name[16];
    r->tid = syscall(SYS_gettid);
    snprintf(name, sizeof name, "%.13s-%d", name_prefix, idx);
    pthread_setname_np(pthread_self(), name); /* comm shows up per thread in perf, bpftrace and ps -L */
    if (sp->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(sp->cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof set, &set)) {
            perror("pthread_setaffinity_np");
            r->failed = 1;
        }
    }
    if (apply_policy(sp, r->tid)) {
        int e = errno;
        fprintf(stderr, "%s: sched=%s: %s%s\n", name, sp->policy, strerror(e),
                e == EBUSY ? "  <- admission control refused the reservation" :
                e == EPERM ? "  (root needed; a dl thread must not be pinned)" : "");
        r->failed = 1;
    }
    pthread_barrier_wait(&barrier); /* all threads start the measured interval together */
    uint64_t e0 = 0, w0 = 0, e1 = 0, w1 = 0;
    struct rusage u0, u1;
    read_schedstat(r->tid, &e0, &w0);
    getrusage(RUSAGE_THREAD, &u0);
    uint64_t t0 = start_ns, end = t0 + run_ns; /* KEY common window: a starved thread does not get its own late start */
    if (sp->periodic) {
        run_periodic(sp, r, t0, end);
    } else {
        while (now_ns() < end) {
            r->rounds += work_item(1000000); /* 1 ms chunks */
        }
    }
    getrusage(RUSAGE_THREAD, &u1);
    read_schedstat(r->tid, &e1, &w1);
    r->last_cpu = sched_getcpu();
    r->cpu_ms = (u1.ru_utime.tv_sec - u0.ru_utime.tv_sec) * 1e3 + (u1.ru_utime.tv_usec - u0.ru_utime.tv_usec) / 1e3 +
                (u1.ru_stime.tv_sec - u0.ru_stime.tv_sec) * 1e3 + (u1.ru_stime.tv_usec - u0.ru_stime.tv_usec) / 1e3;
    r->exec_ns = e1 - e0;
    r->wait_ns = w1 - w0;
    r->vol = u1.ru_nvcsw - u0.ru_nvcsw;
    r->invol = u1.ru_nivcsw - u0.ru_nivcsw;
    return NULL;
}

int main(int argc, char **argv) {
    double seconds = 5;
    int opt;
    while ((opt = getopt(argc, argv, "d:n:h")) != -1) {
        if (opt == 'd') {
            seconds = atof(optarg);
        } else if (opt == 'n') {
            strncpy(name_prefix, optarg, sizeof name_prefix - 1);
        } else {
            fprintf(stderr, "usage: %s [-d seconds] [-n NAME] <spec>...\n"
                            "  spec: busy|periodic[:period_us[:work_us]][,cpu=N][,sched=other|batch|idle|fifo:P|rr:P|dl:R/P][,nice=N]\n",
                    argv[0]);
            return 1;
        }
    }
    int n = argc - optind;
    if (n < 1 || n > MAX_THREADS) {
        fprintf(stderr, "give 1..%d thread specs\n", MAX_THREADS);
        return 1;
    }
    run_ns = (uint64_t)(seconds * 1e9);
    char *raw[MAX_THREADS];
    for (int i = 0; i < n; i++) {
        raw[i] = strdup(argv[optind + i]);
        if (parse_spec(argv[optind + i], &specs[i])) {
            fprintf(stderr, "bad spec: %s\n", raw[i]);
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
    start_ns = now_ns(); /* published to the threads by the barrier */
    pthread_barrier_wait(&barrier);
    /* threads have set affinity and policy; publish ids for external tools */
    printf("pid=%d name=%s duration=%.1fs threads=%d\nTIDS=", (int)getpid(), name_prefix, seconds, n);
    for (int i = 0; i < n; i++) {
        printf("%s%ld", i ? "," : "", results[i].tid);
    }
    printf("\n");
    fflush(stdout);
    for (int i = 0; i < n; i++) {
        pthread_join(th[i], NULL);
    }
    printf("%-3s %-34s %7s %4s %8s %8s %6s %6s %10s %6s %9s %9s %9s %5s\n", "idx", "spec", "tid", "cpu", "cpu_ms",
           "wait_ms", "vol", "invol", "rounds", "iters", "late_p99u", "resp_p99u", "resp_maxu", "miss");
    double total = 0;
    for (int i = 0; i < n; i++) {
        struct result *r = &results[i];
        if (specs[i].periodic) {
            printf("%-3d %-34.34s %7ld %4d %8.1f %8.1f %6ld %6ld %10llu %6llu %9.1f %9.1f %9.1f %5llu%s\n", i, raw[i], r->tid,
                   r->last_cpu, r->cpu_ms, r->wait_ns / 1e6, r->vol, r->invol, (unsigned long long)r->rounds,
                   (unsigned long long)r->iters, r->late_p99_ns / 1e3, r->resp_p99_ns / 1e3, r->resp_max_ns / 1e3,
                   (unsigned long long)r->misses, r->failed ? "  (setup failed)" : "");
        } else {
            printf("%-3d %-34.34s %7ld %4d %8.1f %8.1f %6ld %6ld %10llu %6s %9s %9s %9s %5s%s\n", i, raw[i], r->tid,
                   r->last_cpu, r->cpu_ms, r->wait_ns / 1e6, r->vol, r->invol, (unsigned long long)r->rounds, "-", "-", "-",
                   "-", "-", r->failed ? "  (setup failed)" : "");
        }
        total += r->cpu_ms;
    }
    printf("total cpu_ms=%.1f  (%.2f CPUs over %.1fs)\n", total, total / (seconds * 1000.0), seconds);
    return 0;
}
