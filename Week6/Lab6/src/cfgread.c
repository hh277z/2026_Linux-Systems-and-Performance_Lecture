/*
 * cfgread.c - 읽기가 대부분인 설정값을 rwlock과 RCU로 보호할 때 (실습 2 4단계)
 *
 *   ./bin/cfgread -m rwlock|rcu|nogp [-r readers] [-d seconds] [-u update_us]
 *
 * reader thread들이 설정(struct config)을 계속 읽고, writer thread 하나가 update_us마다 timeout을
 * 3과 5로 바꾼다 (강의의 timeout 예제).
 *
 *   rwlock  reader는 읽을 때마다 read lock을 잡는다. writer는 write lock을 잡고 그 자리에서 고친다
 *   rcu     reader는 pointer를 load해서 읽기만 한다. writer는 새 copy를 만들어 pointer를 바꾸고,
 *           grace period가 지난 뒤 옛 copy를 free한다
 *   nogp    rcu와 같지만 writer가 pointer를 바꾸자마자 옛 copy를 free한다 (일부러 틀린 버전)
 *
 * RCU는 user space에서 흔히 쓰는 QSBR(quiescent-state-based) 방식을 단순하게 만든 것이다.
 * reader는 읽기 100번(event loop 한 바퀴에 해당)마다 "여기까지 읽은 것은 끝났다"를 자기 counter에
 * 적는다(quiescent state). writer는 pointer를 바꾼 뒤 모든 reader가 quiescent state를 한 번 지날
 * 때까지 기다린다. kernel의 non-preemptible RCU에서 context switch가 하는 역할이다.
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

#define MAX_READERS 16
#define NFIELD 8
#define BATCH 100                                    /* reader가 quiescent state를 알리는 간격 (읽기 횟수) */
#define MAX_UPDATES 200000

struct config {                                      /* writer는 모든 field를 같은 값으로 쓴다 */
    int timeout;
    int field[NFIELD - 1];
};

enum mode { RWLOCK, RCU, NOGP };
static const char *mode_name[] = { "rwlock", "rcu", "nogp" };
static enum mode mode = RCU;
static int nreaders = 2, update_us = 1000;
static double seconds = 2;
static int ncpu, cpus[CPU_SETSIZE];
static atomic_int stop, ready, go;

/* ---- rwlock 버전의 공유 데이터 ------------------------------------------ */

static pthread_rwlock_t cfg_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct config cfg_inplace = { 3, { 3, 3, 3, 3, 3, 3, 3 } };

/* ---- rcu, nogp 버전의 공유 데이터 ---------------------------------------- */

static struct { _Atomic(struct config *) ptr; } __attribute__((aligned(64))) gp;   /* 현재 설정을 가리킨다 */
static struct { atomic_ulong seq; } __attribute__((aligned(64))) gp_seq = { 1 };   /* grace period 번호 */
static struct { atomic_ulong seen; } __attribute__((aligned(64))) qs[MAX_READERS]; /* reader마다 자기 line */

/* ---- 결과 ------------------------------------------------------------- */

static long reads[MAX_READERS], bad[MAX_READERS];
static uint64_t upd_ns[MAX_UPDATES], gp_ns[MAX_UPDATES];
static long nupdates;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int check(const struct config *c)            /* field가 모두 같고 3 또는 5여야 한다 */
{
    int v = c->timeout, ok = (v == 3 || v == 5);
    for (int i = 0; i < NFIELD - 1; i++)
        ok &= (c->field[i] == v);
    return ok;
}

/* ---- reader ----------------------------------------------------------- */

static void quiescent_state(long id)
{
    unsigned long seq = atomic_load_explicit(&gp_seq.seq, memory_order_acquire);
    atomic_store_explicit(&qs[id].seen, seq, memory_order_release);   /* KEY quiescent state: 지금까지 읽은 것은 끝났다 */
}

static void *reader(void *arg)
{
    long id = (long)arg;
    const enum mode m = mode;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpus[id % ncpu], &set);
    pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    char name[16];
    snprintf(name, sizeof name, "reader-%ld", id);
    pthread_setname_np(pthread_self(), name);

    quiescent_state(id);
    atomic_fetch_add(&ready, 1);
    while (!atomic_load_explicit(&go, memory_order_acquire))
        ;
    long n = 0, b = 0;
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) {
        for (int k = 0; k < BATCH; k++) {
            if (m == RWLOCK) {
                pthread_rwlock_rdlock(&cfg_lock);    /* KEY reader도 lock 상태(reader 수)를 atomic으로 쓴다 */
                b += !check(&cfg_inplace);
                pthread_rwlock_unlock(&cfg_lock);
            } else {
                struct config *c = atomic_load_explicit(&gp.ptr, memory_order_acquire);   /* KEY RCU reader: pointer를 load할 뿐 아무것도 쓰지 않는다 */
                b += !check(c);
            }
            n++;
        }
        if (m != RWLOCK)
            quiescent_state(id);
    }
    reads[id] = n;
    bad[id] = b;
    return NULL;
}

/* ---- writer ----------------------------------------------------------- */

static int synchronize_rcu(void)                   /* grace period가 끝났으면 1. 종료 중이라 그만두었으면 0 */
{
    unsigned long target = atomic_fetch_add_explicit(&gp_seq.seq, 1, memory_order_acq_rel) + 1;   /* 새 grace period 시작 */
    for (int i = 0; i < nreaders; i++)
        while (atomic_load_explicit(&qs[i].seen, memory_order_acquire) != target) {   /* KEY 모든 reader가 quiescent state를 지날 때까지 */
            if (atomic_load_explicit(&stop, memory_order_relaxed))
                return 0;
            usleep(50);                              /* 기다리는 동안 writer는 잠든다 (synchronize_rcu도 잠든다) */
        }
    return 1;
}

static void *writer(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "writer");
    int v = 3;
    while (!atomic_load(&go))
        ;
    while (!atomic_load_explicit(&stop, memory_order_relaxed) && nupdates < MAX_UPDATES) {
        usleep((useconds_t)update_us);
        v = (v == 3) ? 5 : 3;
        uint64_t t0 = now_ns(), g0 = 0;
        if (mode == RWLOCK) {
            pthread_rwlock_wrlock(&cfg_lock);        /* 모든 reader가 나갈 때까지 기다린다 */
            cfg_inplace.timeout = v;
            for (int i = 0; i < NFIELD - 1; i++)
                cfg_inplace.field[i] = v;
            pthread_rwlock_unlock(&cfg_lock);
        } else {
            struct config *old = atomic_load_explicit(&gp.ptr, memory_order_relaxed);
            struct config *nw = malloc(sizeof *nw);
            nw->timeout = v;                         /* 1 copy: 아직 아무도 못 보는 새 copy를 고친다 */
            for (int i = 0; i < NFIELD - 1; i++)
                nw->field[i] = v;
            atomic_store_explicit(&gp.ptr, nw, memory_order_release);   /* 2 publish: pointer store 한 번 */
            g0 = now_ns();
            if (mode == RCU && !synchronize_rcu())   /* 3 grace period: 옛 copy를 읽던 reader가 모두 끝날 때까지 */
                break;                               /* 종료 중: grace period가 끝나지 않았으니 free하지 않는다 */
            memset(old, 0xff, sizeof *old);          /* 해제한 copy를 읽으면 드러나도록 값을 망가뜨린다 */
            free(old);                               /* KEY 4 reclaim. nogp는 grace period 없이 바로 여기로 온다 */
        }
        uint64_t t1 = now_ns();
        upd_ns[nupdates] = t1 - t0;
        gp_ns[nupdates] = g0 ? t1 - g0 : 0;
        nupdates++;
    }
    return NULL;
}

/* ---- main ------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int usage(void)
{
    fprintf(stderr, "usage: cfgread -m rwlock|rcu|nogp [-r readers(1~%d)] [-d seconds] [-u update_us]\n", MAX_READERS);
    return 2;
}

int main(int argc, char **argv)
{
    int opt, m = -1;
    while ((opt = getopt(argc, argv, "m:r:d:u:")) != -1) {
        switch (opt) {
        case 'm':
            for (int i = 0; i < 3; i++)
                if (!strcmp(optarg, mode_name[i]))
                    m = i;
            if (m < 0) {
                fprintf(stderr, "cfgread: 모르는 모드 '%s'\n", optarg);
                return usage();
            }
            break;
        case 'r': nreaders = atoi(optarg); break;
        case 'd': seconds = atof(optarg); break;
        case 'u': update_us = atoi(optarg); break;
        default:  return usage();
        }
    }
    if (m < 0 || nreaders < 1 || nreaders > MAX_READERS || seconds <= 0 || update_us < 0)
        return usage();
    mode = (enum mode)m;

    cpu_set_t allowed;
    sched_getaffinity(0, sizeof allowed, &allowed);
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &allowed))
            cpus[ncpu++] = c;

    struct config *c0 = malloc(sizeof *c0);
    *c0 = cfg_inplace;
    atomic_store(&gp.ptr, c0);

    printf("cfgread: mode=%s readers=%d cpus=%d seconds=%g update_us=%d\n", mode_name[mode], nreaders, ncpu, seconds, update_us);
    pthread_t rt[MAX_READERS], wt;
    for (long i = 0; i < nreaders; i++)
        pthread_create(&rt[i], NULL, reader, (void *)i);
    pthread_create(&wt, NULL, writer, NULL);
    while (atomic_load(&ready) < nreaders)
        usleep(1000);
    uint64_t t0 = now_ns();
    atomic_store_explicit(&go, 1, memory_order_release);
    usleep((useconds_t)(seconds * 1e6));
    atomic_store(&stop, 1);
    for (int i = 0; i < nreaders; i++)
        pthread_join(rt[i], NULL);
    pthread_join(wt, NULL);
    uint64_t elapsed = now_ns() - t0;

    long n = 0, b = 0;
    for (int i = 0; i < nreaders; i++) {
        n += reads[i];
        b += bad[i];
    }
    qsort(upd_ns, (size_t)nupdates, sizeof upd_ns[0], cmp_u64);
    qsort(gp_ns, (size_t)nupdates, sizeof gp_ns[0], cmp_u64);
    printf("reads=%ld ns_per_read=%.1f bad_reads=%ld\n", n, n ? (double)elapsed * nreaders / (double)n : 0.0, b);
    if (nupdates == 0) {
        printf("updates=0\n");
        return 0;
    }
    printf("updates=%ld update_us: p50=%.0f max=%.0f\n", nupdates,
           (double)upd_ns[nupdates / 2] / 1e3, (double)upd_ns[nupdates - 1] / 1e3);
    if (mode == RCU)
        printf("grace_period_us: p50=%.0f max=%.0f\n", (double)gp_ns[nupdates / 2] / 1e3, (double)gp_ns[nupdates - 1] / 1e3);
    return 0;
}
