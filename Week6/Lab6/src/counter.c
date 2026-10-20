/*
 * counter.c - 여러 thread가 counter 하나를 올릴 때의 결과와 비용 (실습 1)
 *
 *   ./bin/counter [-m plain|atomic|mutex|local] [-t threads] [-n increments]
 *
 *   plain   공유 변수를 그대로 ++ 한다. data race가 있는 잘못된 프로그램의 예다
 *   atomic  atomic_fetch_add로 올린다
 *   mutex   한 번 올릴 때마다 mutex를 잡는다
 *   local   thread마다 자기 cache line에 있는 counter를 올리고, 끝난 뒤 더한다
 *
 * -n은 thread 하나가 올리는 횟수다. thread i는 CPU (i % CPU 수)에 묶인다.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 16
#define NOINLINE __attribute__((noinline))   /* objdump로 함수 하나씩 볼 수 있게 inline하지 않는다 */

/*
 * plain과 local의 counter는 volatile이다. volatile이 없으면 compiler가 loop 전체를
 * "counter += n" 한 번으로 줄여 버린다. volatile은 이 최적화만 막을 뿐 원자성이나
 * thread 사이의 순서를 보장하지 않으므로, plain은 여전히 data race다.
 */
volatile long plain_counter;
atomic_long atomic_counter;
long mutex_counter;
pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

struct slot {                                  /* 64 byte = cache line 하나 */
    volatile long value;
    char pad[64 - sizeof(long)];
};
struct slot local_slots[MAX_THREADS] __attribute__((aligned(64)));

/* ---- 모드별로 thread가 실행하는 loop ---------------------------------- */

NOINLINE void add_plain(long n)
{
    for (long i = 0; i < n; i++)
        plain_counter++;                       /* KEY load, add, store 세 단계라 다른 thread가 끼어들 수 있다 */
}

NOINLINE void add_atomic(long n)
{
    for (long i = 0; i < n; i++)
        atomic_fetch_add(&atomic_counter, 1);  /* KEY 원자적 read-modify-write 한 번 */
}

NOINLINE void add_mutex(long n)
{
    for (long i = 0; i < n; i++) {
        pthread_mutex_lock(&counter_lock);
        mutex_counter++;
        pthread_mutex_unlock(&counter_lock);
    }
}

NOINLINE void add_local(struct slot *s, long n)
{
    for (long i = 0; i < n; i++)
        s->value++;                            /* KEY 자기 cache line만 쓰므로 다른 CPU와 line을 주고받지 않는다 */
}

/* ---- 실행 ------------------------------------------------------------- */

enum mode { PLAIN, ATOMIC, MUTEX, LOCAL };
static const char *mode_name[] = { "plain", "atomic", "mutex", "local" };

static enum mode mode = PLAIN;
static long increments = 5000000;
static pthread_barrier_t start;
static atomic_int go;                         /* main이 계측을 시작한 뒤 worker를 출발시킨다 */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *worker(void *arg)
{
    long id = (long)arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((int)(id % sysconf(_SC_NPROCESSORS_ONLN)), &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);

    pthread_barrier_wait(&start);              /* 모든 thread의 준비가 끝날 때까지 기다린다 */
    while (!atomic_load_explicit(&go, memory_order_acquire))
        ;                                     /* barrier 통과만으로는 측정을 시작하지 않는다 */
    switch (mode) {
    case PLAIN:  add_plain(increments); break;
    case ATOMIC: add_atomic(increments); break;
    case MUTEX:  add_mutex(increments); break;
    case LOCAL:  add_local(&local_slots[id], increments); break;
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int threads = 2, opt;
    while ((opt = getopt(argc, argv, "m:t:n:")) != -1) {
        switch (opt) {
        case 'm':
            for (int i = 0; i < 4; i++)
                if (!strcmp(optarg, mode_name[i]))
                    mode = (enum mode)i;
            break;
        case 't': threads = atoi(optarg); break;
        case 'n': increments = atol(optarg); break;
        default:
            fprintf(stderr, "usage: counter [-m plain|atomic|mutex|local] [-t threads] [-n increments]\n");
            return 2;
        }
    }
    if (threads < 1 || threads > MAX_THREADS)
        threads = 2;

    pthread_t t[MAX_THREADS];
    pthread_barrier_init(&start, NULL, (unsigned)threads + 1);
    for (long i = 0; i < threads; i++)
        pthread_create(&t[i], NULL, worker, (void *)i);
    pthread_barrier_wait(&start);
    uint64_t t0 = now_ns();
    atomic_store_explicit(&go, 1, memory_order_release);
    for (int i = 0; i < threads; i++)
        pthread_join(t[i], NULL);
    uint64_t elapsed = now_ns() - t0;

    long result = 0;
    switch (mode) {
    case PLAIN:  result = plain_counter; break;
    case ATOMIC: result = atomic_counter; break;
    case MUTEX:  result = mutex_counter; break;
    case LOCAL:
        for (int i = 0; i < threads; i++)
            result += local_slots[i].value;
        break;
    }
    long expected = increments * threads;
    printf("counter: mode=%s threads=%d increments/thread=%ld\n", mode_name[mode], threads, increments);
    printf("result=%ld expected=%ld lost=%ld\n", result, expected, expected - result);
    printf("elapsed_ms=%.1f ns_per_increment=%.2f\n", (double)elapsed / 1e6, (double)elapsed / (double)expected);
    return 0;
}
