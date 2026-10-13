/* pi_demo: priority inversion and priority inheritance with three SCHED_FIFO threads on CPU 0 (lecture slides 20-21).
 *
 * usage: sudo ./bin/pi_demo [-p none|inherit] [-r rounds] [-L hold_ms] [-M busy_ms]
 *   -p  mutex protocol: none = plain mutex (default), inherit = PTHREAD_PRIO_INHERIT
 *   -r  rounds (default 5), one round every 500 ms
 *   -L  how long pi-L holds the lock, in CPU ms (default 30)
 *   -M  how long pi-M spins, in CPU ms (default 100)
 *
 * every round:  t+0  pi-L (FIFO 10) takes the lock and works for L ms
 *               t+5  pi-H (FIFO 30) wants the lock -> blocks on pi-L
 *               t+10 pi-M (FIFO 20) wakes and spins for M ms; it does not touch the lock
 * output: how long pi-H waited for the lock in each round.
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
#include <sys/syscall.h>

#define MAX_ROUNDS 100
#define ROUND_NS 500000000ull

static pthread_mutex_t lock;
static int rounds = 5;
static uint64_t hold_ns = 30000000ull, busy_ns = 100000000ull, t0;
static uint64_t h_wait[MAX_ROUNDS], l_hold[MAX_ROUNDS];
static long tids[3];
static pthread_barrier_t ready;

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

/* ns of this thread's CPU time: time spent preempted does not count */
__attribute__((noinline)) void work_cpu(uint64_t ns) {
    volatile uint64_t state = 1;
    uint64_t end = thread_cpu_ns() + ns;
    while (thread_cpu_ns() < end) {
        for (int i = 0; i < 4096; i++) {
            state = state * UINT64_C(6364136223846793005) + 1;
        }
    }
}

static void sleep_until(uint64_t t) {
    struct timespec ts = { .tv_sec = (time_t)(t / 1000000000ull), .tv_nsec = (long)(t % 1000000000ull) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

struct role {
    const char *name;
    int prio;
    int idx;
};

static void setup_self(const struct role *ro) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    pthread_setname_np(pthread_self(), ro->name);
    tids[ro->idx] = syscall(SYS_gettid);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set)) { /* KEY all three share CPU 0 */
        fprintf(stderr, "%s: affinity failed\n", ro->name);
        exit(1);
    }
    struct sched_param p = { .sched_priority = ro->prio };
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &p); /* KEY strict priority: H 30 > M 20 > L 10 */
    if (rc) {
        fprintf(stderr, "%s: SCHED_FIFO %d: %s (run with sudo)\n", ro->name, ro->prio, strerror(rc));
        exit(1);
    }
}

static void *low(void *arg) {
    setup_self(arg);
    pthread_barrier_wait(&ready);
    for (int r = 0; r < rounds; r++) {
        sleep_until(t0 + r * ROUND_NS);
        pthread_mutex_lock(&lock); /* KEY L owns the lock */
        uint64_t s = now_ns();
        work_cpu(hold_ns);
        l_hold[r] = now_ns() - s;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void *mid(void *arg) {
    setup_self(arg);
    pthread_barrier_wait(&ready);
    for (int r = 0; r < rounds; r++) {
        sleep_until(t0 + r * ROUND_NS + 10000000ull);
        work_cpu(busy_ns); /* KEY M never touches the lock, but outranks L */
    }
    return NULL;
}

static void *high(void *arg) {
    setup_self(arg);
    pthread_barrier_wait(&ready);
    for (int r = 0; r < rounds; r++) {
        sleep_until(t0 + r * ROUND_NS + 5000000ull);
        uint64_t s = now_ns();
        pthread_mutex_lock(&lock); /* KEY H blocks here until L unlocks */
        h_wait[r] = now_ns() - s;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int inherit = 0, opt;
    while ((opt = getopt(argc, argv, "p:r:L:M:h")) != -1) {
        if (opt == 'p') {
            inherit = !strcmp(optarg, "inherit");
            if (!inherit && strcmp(optarg, "none")) {
                fprintf(stderr, "-p none|inherit\n");
                return 1;
            }
        } else if (opt == 'r') {
            rounds = atoi(optarg);
        } else if (opt == 'L') {
            hold_ns = (uint64_t)atoi(optarg) * 1000000ull;
        } else if (opt == 'M') {
            busy_ns = (uint64_t)atoi(optarg) * 1000000ull;
        } else {
            fprintf(stderr, "usage: %s [-p none|inherit] [-r rounds] [-L hold_ms] [-M busy_ms]\n", argv[0]);
            return 1;
        }
    }
    if (rounds < 1 || rounds > MAX_ROUNDS || hold_ns + busy_ns + 20000000ull > 900 * 1000000ull) {
        fprintf(stderr, "rounds 1..%d, and L + M must stay well under one round\n", MAX_ROUNDS);
        return 1;
    }
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    if (inherit) {
        pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT); /* KEY PI futex: the kernel records the owner */
    }
    pthread_mutex_init(&lock, &a);

    static struct role roles[3] = { { "pi-L", 10, 0 }, { "pi-M", 20, 1 }, { "pi-H", 30, 2 } };
    void *(*fn[3])(void *) = { low, mid, high };
    pthread_t th[3];
    pthread_barrier_init(&ready, NULL, 4);
    for (int i = 0; i < 3; i++) {
        if (pthread_create(&th[i], NULL, fn[i], &roles[i])) {
            perror("pthread_create");
            return 1;
        }
    }
    t0 = now_ns() + 300000000ull; /* first round starts 300 ms from now */
    pthread_barrier_wait(&ready);
    printf("pid=%d protocol=%s rounds=%d L_hold=%llums M_busy=%llums\n", (int)getpid(), inherit ? "inherit" : "none",
           rounds, (unsigned long long)(hold_ns / 1000000), (unsigned long long)(busy_ns / 1000000));
    printf("TIDS pi-L=%ld pi-M=%ld pi-H=%ld   (kernel prio = 99 - rt_priority: L 89, M 79, H 69)\n", tids[0], tids[1], tids[2]);
    fflush(stdout);
    for (int i = 0; i < 3; i++) {
        pthread_join(th[i], NULL);
    }
    printf("%-6s %12s %14s\n", "round", "H_wait_ms", "L_hold_wall_ms");
    double sum = 0, max = 0;
    for (int r = 0; r < rounds; r++) {
        double w = h_wait[r] / 1e6;
        printf("%-6d %12.2f %14.2f\n", r, w, l_hold[r] / 1e6);
        sum += w;
        max = w > max ? w : max;
    }
    printf("H waited avg %.2f ms, max %.2f ms  (protocol=%s)\n", sum / rounds, max, inherit ? "inherit" : "none");
    return 0;
}
