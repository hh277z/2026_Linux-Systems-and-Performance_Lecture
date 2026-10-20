/*
 * cvbug.c - condition variable과 mutex 사용 오류 세 가지 (실습 3)
 *
 *   ./bin/cvbug -m ifwait|iolock|nounlock|fixed [-n items] [-s slow_ms]
 *
 * producer 하나가 item 0 .. n-1을 크기 4인 queue에 넣고 consumer 둘이 꺼낸다.
 * consumer는 강의 CHECKPOINT의 consumer에서 모드마다 버그 하나만 남긴 것이다.
 *
 *   ifwait    while 대신 if로 한 번만 확인하고 기다린다  -> 빈 queue에서 꺼낸다 (잘못된 dequeue)
 *   iolock    lock을 잡은 채 log를 쓴다                   -> 다른 thread가 lock을 오래 기다린다 (지연)
 *   nounlock  bad item에서 unlock하지 않고 return한다     -> 다른 thread가 영원히 기다린다 (hang)
 *   fixed     세 버그를 모두 고친 버전
 *
 * log 한 줄은 /tmp/cvbug-<pid>.log에 쓴다. iolock과 fixed에서는 저장 장치가 느린
 * 상황을 흉내 내려고 한 줄마다 slow_ms(기본 5 ms)를 더 쉰다.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CAP 4
#define MAX_ITEMS 100000

enum mode { IFWAIT, IOLOCK, NOUNLOCK, FIXED };
static const char *mode_name[] = { "ifwait", "iolock", "nounlock", "fixed" };
static enum mode mode = FIXED;
static int n_items = 200, slow_ms = 5;
static int log_fd = -1;

/* ---- 공유 queue: lock이 아래 모든 field를 보호한다 ------------------- */

static struct {
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    int buf[CAP];
    int head, tail;
    int count;                 /* queue에 든 item 수. 버그가 있으면 음수가 될 수 있다 */
    int done;                  /* producer가 모든 item을 넣었다 */
} q = { .lock = PTHREAD_MUTEX_INITIALIZER, .not_empty = PTHREAD_COND_INITIALIZER, .not_full = PTHREAD_COND_INITIALIZER };

static void put_item(int item)                 /* lock을 잡고 부른다 */
{
    q.buf[q.tail] = item;
    q.tail = (q.tail + 1) % CAP;
    q.count++;
}

static int take_item(void)                     /* lock을 잡고 부른다. 비어 있는지는 검사하지 않는다 */
{
    int item = q.buf[q.head];
    q.head = (q.head + 1) % CAP;
    q.count--;
    return item;
}

/* ---- 기록 ------------------------------------------------------------- */

static int seen[MAX_ITEMS];                    /* item마다 꺼낸 횟수 */
static long empty_takes;                       /* 빈 queue에서 꺼낸 횟수 */
static uint64_t lock_wait_ns[MAX_ITEMS];       /* producer의 pthread_mutex_lock 호출 지연 */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void write_log(int item)
{
    char line[64];
    int len = snprintf(line, sizeof line, "item %d\n", item);
    if (write(log_fd, line, (size_t)len) < 0)
        perror("write");
    if (mode == IOLOCK || mode == FIXED)
        usleep((useconds_t)slow_ms * 1000);    /* 느린 저장 장치 */
}

static int is_bad(int item) { return item % 7 == 6; }

/* ---- producer: 모든 모드에서 같고, 버그가 없다 ------------------------ */

static void *producer(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "producer");
    for (int i = 0; i < n_items; i++) {
        uint64_t t0 = now_ns();
        pthread_mutex_lock(&q.lock);
        lock_wait_ns[i] = now_ns() - t0;
        while (q.count >= CAP)
            pthread_cond_wait(&q.not_full, &q.lock);
        put_item(i);
        if (mode == IFWAIT)
            pthread_cond_broadcast(&q.not_empty);  /* 기다리는 consumer 둘을 함께 깨운다 */
        else
            pthread_cond_signal(&q.not_empty);
        pthread_mutex_unlock(&q.lock);
        usleep(500);                           /* item을 0.5 ms 간격으로 만든다. ifwait에서는 consumer 둘이 다시 잠들 시간이 된다 */
    }
    pthread_mutex_lock(&q.lock);
    q.done = 1;
    pthread_cond_broadcast(&q.not_empty);
    pthread_mutex_unlock(&q.lock);
    return NULL;
}

/* ---- consumer: 모드마다 버그 하나 ------------------------------------- */

static void *consumer(void *arg)
{
    char name[16];
    snprintf(name, sizeof name, "cons-%ld", (long)arg);
    pthread_setname_np(pthread_self(), name);

    for (;;) {
        pthread_mutex_lock(&q.lock);
        if (mode == IFWAIT) {
            if (q.count <= 0 && !q.done)       /* KEY 버그 1: 깨어난 뒤 다시 확인하지 않는다 */
                pthread_cond_wait(&q.not_empty, &q.lock);
        } else {
            while (q.count <= 0 && !q.done)    /* 깨어나면 다시 확인한다 */
                pthread_cond_wait(&q.not_empty, &q.lock);
        }
        if (q.count <= 0 && q.done) {          /* 더 꺼낼 것이 없다 */
            pthread_mutex_unlock(&q.lock);
            return NULL;
        }

        int item = take_item();
        if (q.count < 0)
            empty_takes++;                     /* 빈 queue에서 꺼냈다: item은 예전 값이다 */
        seen[item]++;
        pthread_cond_signal(&q.not_full);

        if (mode == IOLOCK) {
            write_log(item);                   /* KEY 버그 2: lock을 잡은 채 느린 write */
            pthread_mutex_unlock(&q.lock);
            continue;
        }
        if (mode == NOUNLOCK && is_bad(item)) { /* bad item은 unlock 누락을 재현할 때만 사용한다 */
            fprintf(stderr, "%s: item %d rejected\n", name, item);
            return NULL;                       /* KEY 버그 3: unlock하지 않고 끝난다 */
        }
        pthread_mutex_unlock(&q.lock);
        write_log(item);                       /* lock 밖에서 쓴다 */
    }
}

/* ---- main ------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    int opt;
    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((opt = getopt(argc, argv, "m:n:s:")) != -1) {
        switch (opt) {
        case 'm':
            for (int i = 0; i < 4; i++)
                if (!strcmp(optarg, mode_name[i]))
                    mode = (enum mode)i;
            break;
        case 'n': n_items = atoi(optarg); break;
        case 's': slow_ms = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: cvbug -m ifwait|iolock|nounlock|fixed [-n items] [-s slow_ms]\n");
            return 2;
        }
    }
    if (n_items < 1 || n_items > MAX_ITEMS)
        n_items = 200;

    char path[64];
    snprintf(path, sizeof path, "/tmp/cvbug-%d.log", getpid());
    log_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    printf("cvbug: mode=%s items=%d consumers=2 slow_ms=%d\n", mode_name[mode], n_items, slow_ms);
    printf("pid=%d\n", getpid());

    pthread_t p, c[2];
    uint64_t t0 = now_ns();
    for (long i = 0; i < 2; i++)
        pthread_create(&c[i], NULL, consumer, (void *)i);
    pthread_create(&p, NULL, producer, NULL);
    pthread_join(p, NULL);
    for (int i = 0; i < 2; i++)
        pthread_join(c[i], NULL);
    uint64_t elapsed = now_ns() - t0;

    long taken = 0, duplicate = 0, lost = 0;
    for (int i = 0; i < n_items; i++) {
        taken += seen[i];
        if (seen[i] == 0)
            lost++;
        else if (seen[i] > 1)
            duplicate += seen[i] - 1;
    }
    qsort(lock_wait_ns, (size_t)n_items, sizeof lock_wait_ns[0], cmp_u64);
    printf("taken=%ld duplicate=%ld lost=%ld empty_takes=%ld\n", taken, duplicate, lost, empty_takes);
    printf("producer lock wait (us): p50=%.0f p99=%.0f max=%.0f\n",
           (double)lock_wait_ns[n_items / 2] / 1e3, (double)lock_wait_ns[(n_items * 99) / 100] / 1e3,
           (double)lock_wait_ns[n_items - 1] / 1e3);
    printf("elapsed_ms=%.0f\n", (double)elapsed / 1e6);
    close(log_fd);
    unlink(path);
    return 0;
}
