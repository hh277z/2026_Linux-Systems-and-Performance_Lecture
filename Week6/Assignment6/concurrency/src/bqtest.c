/*
 * bqtest.c - tests and load generator for the bounded queue (provided, do not modify).
 *
 *   ./bin/bqtest [-p producers] [-c consumers] [-n items] [-q capacity] [-b] [-w work_us] [-T seconds]
 *   ./bin/bqtest close
 *   ./bin/bqtest timeout
 *
 * Stress mode: every producer puts n items with id = (producer << 32) | sequence.
 * Consumers take items until bq_get returns BQ_CLOSED. The main thread joins the
 * producers, then calls bq_close, then joins the consumers. The run fails if any
 * item is lost, duplicated, or invalid, if a consumer sees items of one producer
 * out of sequence (dequeue order), or if BQ_CLOSED arrives before bq_close.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bq.h"
#include "common.h"

#define MAX_THREADS 64

/* ---- watchdog: a hang must end the test with a clear message ------- */

static void on_alarm(int sig)
{
    (void)sig;
    static const char msg[] = "FAIL: the test did not finish in time (hang?). "
                              "Attach gdb or read /proc/<pid>/task/*/wchan to see who waits.\n";
    ssize_t unused = write(2, msg, sizeof msg - 1);
    (void)unused;
    _exit(2);
}

static void start_watchdog(int seconds)
{
    signal(SIGALRM, on_alarm);
    alarm((unsigned)seconds);
}

/* ==== stress mode ===================================================== */

static struct {
    int producers, consumers, items, capacity, flags, work_us;
} cfg = { 2, 2, 100000, 4, 0, 0 };

static bq_t q;
static atomic_int close_called;                /* set just before bq_close          */
static atomic_uchar *seen;                     /* seen[p * items + seq]              */
static atomic_uint *done_ticket;               /* completion order, information only */
static atomic_uint next_ticket;

struct cons_result {
    long received, garbage, duplicate, order_violation, early_closed;
};

static void *producer(void *arg)
{
    int p = (int)(long)arg;
    char name[32];
    snprintf(name, sizeof name, "prod-%d", p);
    set_thread_name(name);

    for (int seq = 0; seq < cfg.items; seq++) {
        bq_item_t it = { .id = ((uint64_t)p << 32) | (uint32_t)seq };
        if (bq_put(&q, it) != BQ_OK) {
            fprintf(stderr, "FAIL: %s: bq_put did not return BQ_OK before bq_close\n", name);
            break;
        }
    }
    return NULL;
}

static void *consumer(void *arg)
{
    struct cons_result *r = arg;
    long last_seq[MAX_THREADS];
    for (int p = 0; p < MAX_THREADS; p++)
        last_seq[p] = -1;

    for (;;) {
        bq_item_t it;
        int rc = bq_get(&q, &it);
        if (rc == BQ_CLOSED) {
            if (!atomic_load(&close_called))
                r->early_closed++;             /* BQ_CLOSED although nobody closed the queue */
            break;
        }
        if (rc != BQ_OK) {
            r->garbage++;
            break;
        }
        uint64_t p = it.id >> 32, seq = it.id & 0xffffffffu;
        if (p >= (uint64_t)cfg.producers || seq >= (uint64_t)cfg.items) {
            r->garbage++;                      /* an id no producer ever made */
            continue;
        }
        r->received++;
        if (atomic_exchange(&seen[p * (uint64_t)cfg.items + seq], 1))
            r->duplicate++;
        if ((long)seq <= last_seq[p])
            r->order_violation++;              /* dequeue order: this consumer saw p's items out of sequence */
        last_seq[p] = (long)seq;

        if (cfg.work_us)
            burn_cpu_us((uint64_t)cfg.work_us);   /* "process" the item */
        atomic_store(&done_ticket[p * (uint64_t)cfg.items + seq], atomic_fetch_add(&next_ticket, 1));
    }
    return NULL;
}

static int run_stress(void)
{
    if (cfg.producers > MAX_THREADS || cfg.consumers > MAX_THREADS) {
        fprintf(stderr, "at most %d producers and %d consumers\n", MAX_THREADS, MAX_THREADS);
        return 2;
    }
    size_t total = (size_t)cfg.producers * (size_t)cfg.items;
    seen = calloc(total, sizeof *seen);
    done_ticket = calloc(total, sizeof *done_ticket);
    if (!seen || !done_ticket || bq_init(&q, cfg.capacity, cfg.flags) != 0) {
        fprintf(stderr, "setup failed\n");
        return 2;
    }

    printf("bqtest: producers=%d consumers=%d items/producer=%d capacity=%d wake=%s work_us=%d\n",
           cfg.producers, cfg.consumers, cfg.items, cfg.capacity,
           (cfg.flags & BQ_BROADCAST) ? "broadcast" : "signal", cfg.work_us);
    printf("pid=%d\n", getpid());
    fflush(stdout);

    pthread_t pt[MAX_THREADS], ct[MAX_THREADS];
    struct cons_result cr[MAX_THREADS];
    memset(cr, 0, sizeof cr);

    uint64_t t0 = now_ns();
    for (int c = 0; c < cfg.consumers; c++) {
        pthread_create(&ct[c], NULL, consumer, &cr[c]);
        char name[32];
        snprintf(name, sizeof name, "cons-%d", c);
        pthread_setname_np(ct[c], name);
    }
    for (int p = 0; p < cfg.producers; p++)
        pthread_create(&pt[p], NULL, producer, (void *)(long)p);

    for (int p = 0; p < cfg.producers; p++)
        pthread_join(pt[p], NULL);
    atomic_store(&close_called, 1);
    bq_close(&q);
    for (int c = 0; c < cfg.consumers; c++)
        pthread_join(ct[c], NULL);
    uint64_t elapsed_ns = now_ns() - t0;

    /* Totals. */
    struct cons_result sum = { 0 };
    for (int c = 0; c < cfg.consumers; c++) {
        sum.received += cr[c].received;
        sum.garbage += cr[c].garbage;
        sum.duplicate += cr[c].duplicate;
        sum.order_violation += cr[c].order_violation;
        sum.early_closed += cr[c].early_closed;
    }
    long unique = 0, inversions = 0;
    for (size_t i = 0; i < total; i++)
        unique += atomic_load(&seen[i]);
    for (int p = 0; p < cfg.producers; p++)
        for (int s = 1; s < cfg.items; s++) {
            size_t i = (size_t)p * (size_t)cfg.items + (size_t)s;
            if (atomic_load(&seen[i]) && atomic_load(&seen[i - 1]) &&
                atomic_load(&done_ticket[i]) < atomic_load(&done_ticket[i - 1]))
                inversions++;
        }
    long lost = (long)total - unique;

    pthread_mutex_lock(&q.lock);
    uint64_t put_waits = q.n_put_waits, get_waits = q.n_get_waits;
    pthread_mutex_unlock(&q.lock);
    bq_destroy(&q);

    double secs = (double)elapsed_ns / 1e9;
    printf("received=%ld lost=%ld duplicate=%ld garbage=%ld order_violation=%ld early_closed=%ld\n",
           sum.received, lost, sum.duplicate, sum.garbage, sum.order_violation, sum.early_closed);
    printf("put_waits=%llu get_waits=%llu elapsed_ms=%.1f items_per_s=%.0f\n",
           (unsigned long long)put_waits, (unsigned long long)get_waits,
           secs * 1e3, (double)sum.received / secs);
    printf("completion_inversions=%ld (information only: processing completion order, not dequeue order)\n",
           inversions);

    int ok = lost == 0 && sum.duplicate == 0 && sum.garbage == 0 &&
             sum.order_violation == 0 && sum.early_closed == 0;
    printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ==== close tests ===================================================== */

static int failures;

static void check(int cond, const char *test, const char *what)
{
    printf("  %-22s %-4s %s\n", test, cond ? "ok" : "FAIL", what);
    if (!cond)
        failures++;
}

struct waiter {
    bq_t *q;
    int rc;
    uint64_t returned_ns;
};

static void *blocked_get(void *arg)
{
    struct waiter *w = arg;
    bq_item_t it;
    w->rc = bq_get(w->q, &it);
    w->returned_ns = now_ns();
    return NULL;
}

static void *blocked_put(void *arg)
{
    struct waiter *w = arg;
    bq_item_t it = { .id = 999 };
    w->rc = bq_put(w->q, it);
    w->returned_ns = now_ns();
    return NULL;
}

/* Wait until the expected number of callers has entered the queue's wait path.
 * The caller holds q->lock while incrementing n_*_waits and releases it only
 * when pthread_cond_wait atomically begins waiting, so observing the count
 * under the same mutex avoids relying on a fixed startup delay. */
static int wait_for_queue_waiters(bq_t *q, int producers, uint64_t expected,
                                  uint64_t timeout_ms)
{
    uint64_t deadline = now_ns() + timeout_ms * 1000000ull;
    for (;;) {
        pthread_mutex_lock(&q->lock);
        uint64_t observed = producers ? q->n_put_waits : q->n_get_waits;
        pthread_mutex_unlock(&q->lock);
        if (observed >= expected)
            return 1;
        if (now_ns() >= deadline)
            return 0;
        sleep_us(1000);
    }
}

/* Starts n threads running fn, lets them block, closes q, and checks that
 * every one returns BQ_CLOSED within 200 ms. Returns 1 if a waiter is still
 * blocked: the caller must then leave the queue alone, because destroying a
 * queue that still has waiters is not allowed (bq.h). */
static int close_wakes_all(bq_t *cq, void *(*fn)(void *), int n, const char *test)
{
    pthread_t t[8];
    struct waiter w[8];
    for (int i = 0; i < n; i++) {
        w[i] = (struct waiter){ cq, -1, 0 };
        pthread_create(&t[i], NULL, fn, &w[i]);
    }
    int producers = (fn == blocked_put);
    int all_waiting = wait_for_queue_waiters(cq, producers, (uint64_t)n, 1000);
    check(all_waiting, test, "every thread entered the queue wait path before close");
    uint64_t t0 = now_ns();
    bq_close(cq);

    int all_closed = 1, all_fast = all_waiting;
    for (int i = 0; i < n; i++) {
        struct timespec limit;
        clock_gettime(CLOCK_REALTIME, &limit);
        limit.tv_sec += 1;
        if (pthread_timedjoin_np(t[i], NULL, &limit) != 0) {
            check(0, test, "a waiter was still blocked 1 s after bq_close");
            return 1;
        }
        all_closed &= (w[i].rc == BQ_CLOSED);
        all_fast &= (w[i].returned_ns - t0 < 200000000ull);
    }
    check(all_closed, test, "every waiter returned BQ_CLOSED");
    check(all_fast, test, "every waiter returned within 200 ms of bq_close");
    return 0;
}

static int run_close_tests(void)
{
    bq_t cq;
    bq_item_t it;
    printf("close tests (wake mode: signal)\n");

    /* 1. consumers blocked on an empty queue */
    bq_init(&cq, 4, 0);
    if (!close_wakes_all(&cq, blocked_get, 3, "consumers_wake"))
        bq_destroy(&cq);                       /* else: waiters are still blocked, leave the queue */

    /* 2. producers blocked on a full queue: none of their items may get in */
    bq_t pq;
    bq_init(&pq, 1, 0);
    bq_put(&pq, (bq_item_t){ .id = 1 });
    if (!close_wakes_all(&pq, blocked_put, 3, "producers_wake")) {
        int rc1 = bq_get(&pq, &it);
        check(rc1 == BQ_OK && it.id == 1, "producers_wake", "the item put before close is still there");
        check(bq_get(&pq, &it) == BQ_CLOSED, "producers_wake", "no blocked producer enqueued after close");
        bq_destroy(&pq);
    }

    /* 3. remaining items drain before BQ_CLOSED */
    bq_init(&cq, 4, 0);
    for (uint64_t i = 1; i <= 3; i++)
        bq_put(&cq, (bq_item_t){ .id = i });
    bq_close(&cq);
    int drained = 1;
    for (uint64_t i = 1; i <= 3; i++)
        drained &= (bq_get(&cq, &it) == BQ_OK && it.id == i);
    check(drained, "drain", "3 items come out in order after close");
    check(bq_get(&cq, &it) == BQ_CLOSED, "drain", "then bq_get returns BQ_CLOSED");
    check(bq_timed_get(&cq, &it, 10) == BQ_CLOSED, "drain", "and bq_timed_get returns BQ_CLOSED");
    bq_destroy(&cq);

    /* 4. put after close */
    bq_init(&cq, 4, 0);
    bq_close(&cq);
    check(bq_put(&cq, (bq_item_t){ .id = 1 }) == BQ_CLOSED, "put_after_close", "bq_put returns BQ_CLOSED");
    check(bq_get(&cq, &it) == BQ_CLOSED, "put_after_close", "and nothing was enqueued");
    bq_destroy(&cq);

    /* 5. close twice */
    bq_init(&cq, 4, 0);
    bq_close(&cq);
    bq_close(&cq);
    check(bq_put(&cq, (bq_item_t){ .id = 1 }) == BQ_CLOSED, "close_twice", "second bq_close changes nothing");
    bq_destroy(&cq);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/* ==== timeout tests =================================================== */

struct helper {
    bq_t *q;
    int delay_ms;
    int action;                /* 0 put an item, 1 close, 2 spurious wakeups only */
    atomic_int stop;
};

static void *helper_thread(void *arg)
{
    struct helper *h = arg;
    if (h->action == 2) {
        /* Wake the consumer every 10 ms without giving it an item. A correct
         * bq_timed_get goes back to sleep and still returns at its deadline. */
        while (!atomic_load(&h->stop)) {
            sleep_us(10000);
            pthread_mutex_lock(&h->q->lock);
            pthread_cond_broadcast(&h->q->not_empty);
            pthread_mutex_unlock(&h->q->lock);
        }
        return NULL;
    }
    sleep_us((uint64_t)h->delay_ms * 1000);
    if (h->action == 0)
        bq_put(h->q, (bq_item_t){ .id = 7 });
    else
        bq_close(h->q);
    return NULL;
}

static double timed_get_ms(bq_t *tq, int timeout_ms, int *rc)
{
    bq_item_t it;
    uint64_t t0 = now_ns();
    *rc = bq_timed_get(tq, &it, timeout_ms);
    return (double)(now_ns() - t0) / 1e6;
}

static int run_timeout_tests(void)
{
    bq_t tq;
    int rc;
    double ms;
    char what[96];
    printf("timeout tests (CLOCK_MONOTONIC)\n");

    bq_init(&tq, 4, 0);
    ms = timed_get_ms(&tq, 50, &rc);
    snprintf(what, sizeof what, "empty queue, 50 ms: BQ_TIMEOUT after 50..150 ms (got %.1f ms)", ms);
    check(rc == BQ_TIMEOUT && ms >= 49.5 && ms < 150, "timeout_50ms", what);

    ms = timed_get_ms(&tq, 0, &rc);
    snprintf(what, sizeof what, "timeout 0: BQ_TIMEOUT without blocking (got %.1f ms)", ms);
    check(rc == BQ_TIMEOUT && ms < 10, "timeout_0", what);

    pthread_t t;
    struct helper h = { &tq, 20, 0, 0 };
    pthread_create(&t, NULL, helper_thread, &h);
    ms = timed_get_ms(&tq, 200, &rc);
    pthread_join(t, NULL);
    snprintf(what, sizeof what, "item put after 20 ms: BQ_OK well before 200 ms (got %.1f ms)", ms);
    check(rc == BQ_OK && ms < 150, "item_arrives", what);

    h = (struct helper){ &tq, 0, 2, 0 };
    pthread_create(&t, NULL, helper_thread, &h);
    ms = timed_get_ms(&tq, 300, &rc);
    atomic_store(&h.stop, 1);
    pthread_join(t, NULL);
    snprintf(what, sizeof what, "wakeups every 10 ms, no item: BQ_TIMEOUT at 300..450 ms (got %.1f ms)", ms);
    check(rc == BQ_TIMEOUT && ms >= 299.5 && ms < 450, "deadline_fixed", what);

    h = (struct helper){ &tq, 20, 1, 0 };
    pthread_create(&t, NULL, helper_thread, &h);
    ms = timed_get_ms(&tq, 200, &rc);
    pthread_join(t, NULL);
    snprintf(what, sizeof what, "closed after 20 ms: BQ_CLOSED well before 200 ms (got %.1f ms)", ms);
    check(rc == BQ_CLOSED && ms < 150, "close_arrives", what);
    bq_destroy(&tq);

    bq_init(&tq, 4, 0);
    bq_put(&tq, (bq_item_t){ .id = 3 });
    bq_close(&tq);
    ms = timed_get_ms(&tq, 50, &rc);
    check(rc == BQ_OK, "item_and_closed", "an available item wins over closed");
    bq_destroy(&tq);

    /* A full queue with a producer blocked in bq_put: taking an item with
     * bq_timed_get must wake that producer. */
    bq_t fq;
    bq_init(&fq, 1, 0);
    bq_put(&fq, (bq_item_t){ .id = 1 });
    struct waiter w = { &fq, -1, 0 };
    pthread_create(&t, NULL, blocked_put, &w);
    int producer_waiting = wait_for_queue_waiters(&fq, 1, 1, 1000);
    check(producer_waiting, "get_wakes_producer", "producer entered the full-queue wait path");
    uint64_t t0 = now_ns();
    ms = timed_get_ms(&fq, 100, &rc);
    struct timespec limit;
    clock_gettime(CLOCK_REALTIME, &limit);
    limit.tv_sec += 1;
    if (pthread_timedjoin_np(t, NULL, &limit) != 0) {
        check(0, "get_wakes_producer", "the producer was still blocked 1 s after bq_timed_get took an item");
    } else {
        snprintf(what, sizeof what, "bq_timed_get took an item and the blocked producer returned (%.1f ms)",
                 (double)(w.returned_ns - t0) / 1e6);
        check(producer_waiting && rc == BQ_OK && w.rc == BQ_OK && w.returned_ns - t0 < 200000000ull, "get_wakes_producer", what);
        bq_destroy(&fq);
    }

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/* ==== main ============================================================ */

static void usage(void)
{
    fprintf(stderr,
            "usage: bqtest [-p producers] [-c consumers] [-n items] [-q capacity] [-b] [-w work_us] [-T seconds]\n"
            "       bqtest close | timeout\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int watchdog_s = 60;
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc > 1 && strcmp(argv[1], "close") == 0) {
        start_watchdog(20);
        return run_close_tests();
    }
    if (argc > 1 && strcmp(argv[1], "timeout") == 0) {
        start_watchdog(20);
        return run_timeout_tests();
    }

    int opt;
    while ((opt = getopt(argc, argv, "p:c:n:q:bw:T:")) != -1) {
        switch (opt) {
        case 'p': cfg.producers = atoi(optarg); break;
        case 'c': cfg.consumers = atoi(optarg); break;
        case 'n': cfg.items = atoi(optarg); break;
        case 'q': cfg.capacity = atoi(optarg); break;
        case 'b': cfg.flags |= BQ_BROADCAST; break;
        case 'w': cfg.work_us = atoi(optarg); break;
        case 'T': watchdog_s = atoi(optarg); break;
        default: usage();
        }
    }
    if (cfg.producers < 1 || cfg.consumers < 1 || cfg.items < 1 || cfg.capacity < 1)
        usage();
    start_watchdog(watchdog_s);
    return run_stress();
}
