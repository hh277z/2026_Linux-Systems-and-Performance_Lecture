/*
 * lockbench.c - 같은 critical section을 spinning lock과 sleeping lock으로 보호할 때 (실습 2)
 *
 *   ./bin/lockbench -m spin|mcs|mutex|adaptive [-t threads] [-n ops] [-c cs_loops] [-w work_loops] [-T max_seconds]
 *
 *   spin      lock word 하나를 모두가 보고 도는 spinlock (강의 슬라이드의 simple spinlock)
 *   mcs       waiter마다 자기 node를 보고 돌고, unlock은 다음 waiter에게만 넘겨주는 MCS lock
 *   mutex     pthread_mutex. 경쟁하면 futex로 잠든다
 *   adaptive  glibc adaptive mutex. 잠들기 전에 잠깐 돈다
 *
 * thread마다 ops번 "lock, critical section(loop cs_loops번), unlock, lock 밖의 일(loop work_loops번)"을
 * 반복한다. thread i는 이 process가 쓸 수 있는 CPU 중 (i % CPU 수)번째에 묶이므로, thread가 CPU보다
 * 많으면 한 CPU를 나눠 쓴다. max_seconds가 지나면 그때까지 한 만큼으로 결과를 낸다.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 64

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()           /* x86 PAUSE: "돌고 있다"는 힌트. 잠들지 않는다 */
#elif defined(__aarch64__)
#define cpu_relax() __asm__ volatile("yield")
#else
#define cpu_relax() do { } while (0)
#endif

/*
 * 실행 중에 값이 바뀌는 변수는 각자 cache line 하나를 혼자 쓰게 한다. struct type에 aligned(64)를 주면
 * 크기도 64 byte가 되므로 다른 변수가 같은 line에 들어오지 않는다. 그래야 lock끼리의 차이만 잰다.
 */

/* ---- spin: lock word 하나 -------------------------------------------- */

static struct { atomic_int word; } __attribute__((aligned(64))) spin;      /* 0 = free, 1 = taken */

static void spin_lock(void)
{
    while (atomic_exchange_explicit(&spin.word, 1, memory_order_acquire))   /* 잡아 본다 */
        while (atomic_load_explicit(&spin.word, memory_order_relaxed))      /* KEY 모두가 같은 word를 보고 돈다 */
            cpu_relax();
}

static void spin_unlock(void)
{
    atomic_store_explicit(&spin.word, 0, memory_order_release);             /* 기다리던 모두의 사본이 무효가 된다 */
}

/* ---- mcs: waiter마다 자기 node ---------------------------------------- */

struct mcs_node {
    _Atomic(struct mcs_node *) next;
    atomic_int wait;                                 /* 1 = 아직 내 차례가 아니다 */
} __attribute__((aligned(64)));                      /* node마다 다른 cache line */

static struct { _Atomic(struct mcs_node *) ptr; } __attribute__((aligned(64))) mcs_tail;   /* 줄의 마지막 waiter */
static struct mcs_node mcs_nodes[MAX_THREADS];

static void mcs_lock(struct mcs_node *me)
{
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&me->wait, 1, memory_order_relaxed);
    struct mcs_node *prev = atomic_exchange_explicit(&mcs_tail.ptr, me, memory_order_acq_rel);   /* 줄 끝에 선다 */
    if (prev == NULL)
        return;                                      /* 아무도 없었다: 바로 잡았다 */
    atomic_store_explicit(&prev->next, me, memory_order_release);
    while (atomic_load_explicit(&me->wait, memory_order_acquire))           /* KEY 자기 node만 보고 돈다 */
        cpu_relax();
}

static void mcs_unlock(struct mcs_node *me)
{
    struct mcs_node *next = atomic_load_explicit(&me->next, memory_order_acquire);
    if (next == NULL) {
        struct mcs_node *expected = me;
        if (atomic_compare_exchange_strong_explicit(&mcs_tail.ptr, &expected, NULL,
                                                    memory_order_acq_rel, memory_order_acquire))
            return;                                  /* 기다리는 thread가 없다 */
        while ((next = atomic_load_explicit(&me->next, memory_order_acquire)) == NULL)
            cpu_relax();                             /* 줄에 서는 중인 thread가 next를 쓸 때까지 */
    }
    atomic_store_explicit(&next->wait, 0, memory_order_release);           /* KEY 다음 waiter 한 명에게만 넘겨준다 */
}

/* ---- mutex, adaptive -------------------------------------------------- */

static struct { pthread_mutex_t m; } __attribute__((aligned(64))) mtx;     /* main에서 종류를 정해 초기화한다 */

/* ---- lock이 보호하는 공유 데이터 ---------------------------------------- */

static struct {
    long counter;                                    /* critical section마다 1씩 올린다 */
    long handoffs;                                   /* lock이 직전과 다른 thread로 넘어간 횟수 */
    int last_owner;                                  /* 직전에 lock을 잡은 thread */
} __attribute__((aligned(64))) shared = { .last_owner = -1 };

/* ---- 실행 ------------------------------------------------------------- */

enum mode { SPIN, MCS, MUTEX, ADAPTIVE };
static const char *mode_name[] = { "spin", "mcs", "mutex", "adaptive" };

/* 아래는 시작 전에 정하고 실행 중에는 읽기만 한다 */
static enum mode mode = MUTEX;
static long ops = 200000;
static int cs_loops = 200, work_loops = 200;
static int ncpu, cpus[CPU_SETSIZE];                  /* 이 process가 쓸 수 있는 CPU 번호 */
static atomic_int stop, ready, go;                   /* 출발 신호는 futex를 쓰지 않으려고 pthread_barrier 대신 쓴다 */

struct result {
    long done;
    long vcsw, ivcsw;                                /* 잠든 횟수, preempt된 횟수 */
    double cpu_ms;
};
static struct result res[MAX_THREADS];

static void busy(int loops)
{
    for (volatile int i = 0; i < loops; i++)
        ;
}

static void lock(enum mode m, long id)
{
    switch (m) {
    case SPIN:  spin_lock(); break;
    case MCS:   mcs_lock(&mcs_nodes[id]); break;
    default:    pthread_mutex_lock(&mtx.m); break;  /* KEY 경쟁이 없으면 CAS 한 번, 있으면 futex로 잠든다 */
    }
}

static void unlock(enum mode m, long id)
{
    switch (m) {
    case SPIN:  spin_unlock(); break;
    case MCS:   mcs_unlock(&mcs_nodes[id]); break;
    default:    pthread_mutex_unlock(&mtx.m); break;
    }
}

static void *worker(void *arg)
{
    long id = (long)arg;
    const enum mode m = mode;                        /* 설정값은 local로 복사해 공유 line을 읽지 않는다 */
    const long n = ops;
    const int cs = cs_loops, work = work_loops;
    char name[16];
    snprintf(name, sizeof name, "lb-%ld", id);
    pthread_setname_np(pthread_self(), name);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpus[id % ncpu], &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
        fprintf(stderr, "warning: %s를 CPU %d에 묶지 못했다\n", name, cpus[id % ncpu]);

    atomic_fetch_add(&ready, 1);
    while (!atomic_load_explicit(&go, memory_order_acquire))   /* 모든 thread가 함께 출발한다 */
        cpu_relax();
    long i;
    for (i = 0; i < n && !atomic_load_explicit(&stop, memory_order_relaxed); i++) {
        lock(m, id);
        shared.counter++;
        if (shared.last_owner != id) {               /* KEY 직전과 다른 thread가 잡았으면 handoff */
            shared.handoffs++;
            shared.last_owner = (int)id;
        }
        busy(cs);                                    /* critical section */
        unlock(m, id);
        busy(work);                                  /* lock 밖의 일 */
    }

    struct rusage ru;
    getrusage(RUSAGE_THREAD, &ru);
    res[id].done = i;
    res[id].vcsw = ru.ru_nvcsw;
    res[id].ivcsw = ru.ru_nivcsw;
    res[id].cpu_ms = (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3
                   + (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
    return NULL;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int usage(void)
{
    fprintf(stderr, "usage: lockbench -m spin|mcs|mutex|adaptive [-t threads(1~%d)] [-n ops] [-c cs_loops] [-w work_loops] [-T max_seconds]\n",
            MAX_THREADS);
    return 2;
}

int main(int argc, char **argv)
{
    int threads = 2, opt, m = -1;
    double max_sec = 5;
    while ((opt = getopt(argc, argv, "m:t:n:c:w:T:")) != -1) {
        switch (opt) {
        case 'm':
            for (int i = 0; i < 4; i++)
                if (!strcmp(optarg, mode_name[i]))
                    m = i;
            if (m < 0) {
                fprintf(stderr, "lockbench: 모르는 모드 '%s'\n", optarg);
                return usage();
            }
            break;
        case 't': threads = atoi(optarg); break;
        case 'n': ops = atol(optarg); break;
        case 'c': cs_loops = atoi(optarg); break;
        case 'w': work_loops = atoi(optarg); break;
        case 'T': max_sec = atof(optarg); break;
        default:  return usage();
        }
    }
    if (m < 0 || threads < 1 || threads > MAX_THREADS || ops < 1 || cs_loops < 0 || work_loops < 0 || max_sec <= 0) {
        fprintf(stderr, "lockbench: -m은 꼭 주어야 하고, -t는 1~%d, -n은 1 이상, -T는 0보다 커야 한다\n", MAX_THREADS);
        return usage();
    }
    mode = (enum mode)m;

    cpu_set_t allowed;                               /* nproc과 같은 기준으로 CPU를 센다 */
    sched_getaffinity(0, sizeof allowed, &allowed);
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &allowed))
            cpus[ncpu++] = c;

    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (mode == ADAPTIVE)
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
    pthread_mutex_init(&mtx.m, &attr);

    printf("lockbench: mode=%s threads=%d cpus=%d ops/thread=%ld cs_loops=%d work_loops=%d\n",
           mode_name[mode], threads, ncpu, ops, cs_loops, work_loops);

    pthread_t t[MAX_THREADS];
    for (long i = 0; i < threads; i++)
        pthread_create(&t[i], NULL, worker, (void *)i);
    while (atomic_load(&ready) < threads)
        usleep(1000);
    uint64_t t0 = now_ns();
    atomic_store_explicit(&go, 1, memory_order_release);

    /* 최대 실행 시간이 지나면 멈추라고 알린다. 그 전에 끝나면 join이 바로 돌아온다 */
    int stopped = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    long add_ns = (long)(max_sec * 1e9);
    deadline.tv_sec += add_ns / 1000000000L;
    deadline.tv_nsec += add_ns % 1000000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    for (int i = 0; i < threads; i++) {
        if (!stopped && pthread_clockjoin_np(t[i], NULL, CLOCK_MONOTONIC, &deadline) == 0)
            continue;
        if (!stopped) {
            atomic_store(&stop, 1);
            stopped = 1;
        }
        pthread_join(t[i], NULL);
    }
    uint64_t elapsed = now_ns() - t0;

    long done = 0, vcsw = 0, ivcsw = 0;
    double cpu_ms = 0;
    for (int i = 0; i < threads; i++) {
        done += res[i].done;
        vcsw += res[i].vcsw;
        ivcsw += res[i].ivcsw;
        cpu_ms += res[i].cpu_ms;
    }
    if (stopped)
        printf("stopped after %g s: done=%ld of %ld ops\n", max_sec, done, ops * threads);
    printf("elapsed_ms=%.0f cpu_ms=%.0f ns_per_op=%.0f\n", (double)elapsed / 1e6, cpu_ms,
           done ? (double)elapsed / (double)done : 0.0);
    printf("sleeps=%ld preempted=%ld handoff=%.1f%%\n", vcsw, ivcsw,
           done ? 100.0 * (double)shared.handoffs / (double)done : 0.0);
    if (shared.counter != done)
        printf("ERROR: shared counter=%ld, done=%ld\n", shared.counter, done);
    return 0;
}
