/*
 * devsvc.c - the Level 3 driver (provided, do not modify).
 *
 *   ./bin/devsvc -c <1|2|3|4> [-t threads] [-d seconds]
 *
 * Starts `threads` workers (default 4) that send requests to one case for
 * `seconds` (default 3), checks the results, and prints
 *   ops=<requests> ops_per_sec=<requests per second> parallel=<p>%
 *   check: ...
 *   result: PASS | FAIL | HANG
 * Exit status: 0 PASS, 1 FAIL, 3 HANG, 2 usage error.
 *
 * parallel: the share of svc_work calls (the work inside the case, see
 * devsvc.h) that found another svc_work call running when they started or
 * ended. It is 0 when the case lets only one request work at a time.
 *
 * Hang detection: the main thread watches every worker. If one of them
 * finishes no request for HANG_SECONDS, it prints what each worker was doing
 * and exits with status 3.
 *
 * Every worker is an RCU reader thread (rcu.h) and reports a quiescent state
 * after every request, outside all locks.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "devsvc.h"
#include "rcu.h"

#define MAX_WORKERS  32
#define HANG_SECONDS 3

static int    ncase;
static int    nthreads = 4;
static double seconds  = 3.0;
static atomic_int stop;

struct worker {
    pthread_t   th;
    int         id;
    atomic_int  tid;           /* kernel thread id, as lockcheck prints it */
    unsigned long rng;
    atomic_long ops;
    atomic_int  done;
    _Atomic(const char *) op;  /* what it is doing now, for the hang report */
    atomic_int  a, b;
    long        published;     /* case 2 */
    long        work, overlap; /* svc_work calls, and those that overlapped another */
} __attribute__((aligned(64)));

static struct worker workers[MAX_WORKERS];

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static __thread struct worker *me;     /* NULL in the main thread */
static atomic_int in_work;             /* svc_work calls running now */

void svc_work(int n)
{
    int before = atomic_fetch_add_explicit(&in_work, 1, memory_order_relaxed);
    for (volatile int i = 0; i < n; i++)
        ;
    int after = atomic_fetch_sub_explicit(&in_work, 1, memory_order_relaxed) - 1;
    if (me) {
        me->work++;
        if (before > 0 || after > 0)
            me->overlap++;
    }
}

/* Work outside every lock, not counted in parallel=. */
static void outside_work(int n)
{
    for (volatile int i = 0; i < n; i++)
        ;
}

static unsigned rnd(struct worker *w)
{
    w->rng ^= w->rng << 13;
    w->rng ^= w->rng >> 7;
    w->rng ^= w->rng << 17;
    return (unsigned)(w->rng >> 11);
}

static void set_op(struct worker *w, const char *op, int a, int b)
{
    atomic_store_explicit(&w->a, a, memory_order_relaxed);
    atomic_store_explicit(&w->b, b, memory_order_relaxed);
    atomic_store_explicit(&w->op, op, memory_order_relaxed);
}

/* ---- check failures ---------------------------------------------------- */

static atomic_long n_fail;

static void fail(const char *fmt, ...)
{
    if (atomic_fetch_add(&n_fail, 1) >= 5)
        return;                                  /* print the first five */
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("check failed: %s\n", buf);
    fflush(stdout);
}

void svc_rwlock_init(pthread_rwlock_t *l)
{
    const char *k = getenv("DEVSVC_RWLOCK");
    pthread_rwlockattr_t a;
    pthread_rwlockattr_init(&a);
    if (k && strcmp(k, "writer") == 0)
        pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(l, &a);
    pthread_rwlockattr_destroy(&a);
}

/* ==== case 1: sinks ===================================================== */

static void req_sink(struct worker *w, long n)
{
    int from = (int)(rnd(w) % NSINKS);
    int to = (int)(rnd(w) % (NSINKS - 1));
    if (to >= from)
        to++;
    set_op(w, "sink_move", from, to);
    sink_move(from, to);

    if ((n & 255) == 0) {
        set_op(w, "sink_total", -1, -1);
        long t = sink_total();
        if (t != NSINKS * SESSIONS_PER_SINK)
            fail("sink_total() = %ld, expected %d", t, NSINKS * SESSIONS_PER_SINK);
    }
}

static void check_sink(void)
{
    long t = 0;
    for (int i = 0; i < NSINKS; i++)
        t += sinks[i].sessions;
    printf("check: sessions %ld of %d, sink_total checked by every worker every 256 requests\n",
           t, NSINKS * SESSIONS_PER_SINK);
    if (t != NSINKS * SESSIONS_PER_SINK)
        fail("sessions at the end: %ld", t);
}

/* ==== case 2: bus ======================================================= */

#define NPERSIST 8                 /* stay subscribed for the whole run */
#define NONESHOT 8                 /* unsubscribe themselves when called */

static struct sub persist[NPERSIST], oneshot[NONESHOT];

static void cb_persist(struct sub *s, int ev)
{
    (void)ev;
    atomic_fetch_add_explicit(&s->calls, 1, memory_order_relaxed);
    svc_work(WORK_CB);
}

static void cb_oneshot(struct sub *s, int ev)
{
    (void)ev;
    atomic_fetch_add_explicit(&s->calls, 1, memory_order_relaxed);
    svc_work(WORK_CB);
    bus_unsubscribe(s);            /* calls back into the bus */
}

static void setup_bus(void)
{
    bus_init();
    for (int i = 0; i < NPERSIST; i++) {           /* P O P O ... */
        persist[i] = (struct sub){ .cb = cb_persist, .id = i };
        oneshot[i] = (struct sub){ .cb = cb_oneshot, .id = 100 + i };
        bus_subscribe(&persist[i]);
        bus_subscribe(&oneshot[i]);
    }
}

static void req_bus(struct worker *w, long n)
{
    set_op(w, "bus_publish", (int)n, -1);
    bus_publish((int)n);
    w->published++;
    struct sub *s = &oneshot[(n + w->id) % NONESHOT];
    set_op(w, "bus_subscribe", s->id, -1);
    bus_subscribe(s);              /* arm a one-shot subscriber again */
}

static void check_bus(void)
{
    long published = 0;
    for (int i = 0; i < nthreads; i++)
        published += workers[i].published;
    long lo = -1, hi = -1;
    for (int i = 0; i < NPERSIST; i++) {
        long c = atomic_load(&persist[i].calls);
        if (lo < 0 || c < lo)
            lo = c;
        if (c > hi)
            hi = c;
        if (c != published)
            fail("persistent subscriber %d was called %ld times for %ld events", i, c, published);
    }
    printf("check: %ld events, every persistent subscriber called %ld to %ld times (must be %ld)\n",
           published, lo, hi, published);
}

/* ==== case 3: catalog =================================================== */

static void req_catalog(struct worker *w, long n)
{
    (void)n;
    unsigned r = rnd(w) % 100;
    int app = (int)(rnd(w) % NAPPS), perm = (int)(rnd(w) % NPERMS);
    if (r < 45) {
        set_op(w, "catalog_can_launch", app, perm);
        catalog_can_launch(app, perm);
    } else if (r < 90) {
        int enabled = -1;
        set_op(w, "catalog_perm_users", perm, -1);
        int users = catalog_perm_users(perm, &enabled);
        if (!enabled && users > 0)
            fail("perm %d is disabled but %d apps hold it", perm, users);
    } else if (r < 93) {
        set_op(w, "catalog_grant", app, perm);
        catalog_grant(app, perm);
    } else if (r < 98) {
        set_op(w, "catalog_revoke", app, perm);
        catalog_revoke(app, perm);
    } else {
        int en = (int)(rnd(w) & 1);
        set_op(w, "catalog_set_perm", perm, en);
        catalog_set_perm(perm, en);
    }
}

static void check_catalog(void)
{
    long bad = 0;
    for (int p = 0; p < NPERMS; p++) {
        int enabled = -1;
        int users = catalog_perm_users(p, &enabled);
        if (!enabled && users > 0)
            bad++;
    }
    printf("check: no app holds a disabled permission (checked in every catalog_perm_users and at the end)\n");
    if (bad)
        fail("%ld disabled permissions are held at the end", bad);
}

/* ==== case 4: config ==================================================== */

static atomic_long bad_frames;

static void req_config(struct worker *w, long n)
{
    (void)n;
    if (rnd(w) % 2000 == 0) {           /* config updates are rare */
        set_op(w, "config_update", -1, -1);
        config_update();
    } else {
        int out = (int)(rnd(w) % NOUTPUTS);
        set_op(w, "output_render", out, -1);
        if (!output_render(out) && atomic_fetch_add(&bad_frames, 1) == 0)
            fail("output_render(%d) read an inconsistent or freed config", out);
    }
}

static void check_config(void)
{
    unsigned v = config_version();
    long frames = 0;
    for (int i = 0; i < NOUTPUTS; i++) {
        frames += outputs[i].frames;
        if (outputs[i].applied != v)
            fail("output %d applied version %u, current is %u", i, outputs[i].applied, v);
    }
    printf("check: %ld frames, %ld with a bad config, config version %u applied to every output\n",
           frames, atomic_load(&bad_frames), v);
}

/* ==== workers and the watchdog ========================================== */

static void (*request)(struct worker *, long);

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    me = w;
    atomic_store(&w->tid, (int)gettid());
    rcu_register_thread();
    for (long n = 0; !atomic_load_explicit(&stop, memory_order_relaxed); n++) {
        request(w, n);
        set_op(w, "between requests", -1, -1);
        rcu_quiescent_state();
        atomic_store_explicit(&w->ops, n + 1, memory_order_relaxed);
        outside_work(WORK_OUTSIDE);
    }
    rcu_unregister_thread();
    atomic_store(&w->done, 1);
    return NULL;
}

static void report_hang(int stuck, double idle)
{
    printf("HANG: worker %d finished no request for %.1f s\n", stuck, idle);
    for (int i = 0; i < nthreads; i++) {
        struct worker *w = &workers[i];
        const char *op = atomic_load(&w->op);
        int a = atomic_load(&w->a), b = atomic_load(&w->b);
        printf("  worker %d (tid %d): %s", i, atomic_load(&w->tid),
               atomic_load(&w->done) ? "finished" : op ? op : "starting");
        if (!atomic_load(&w->done) && a >= 0)
            printf(b >= 0 ? "(%d, %d)" : "(%d)", a, b);
        printf(", %ld requests\n", atomic_load(&w->ops));
    }
    printf("result: HANG\n");
    fflush(stdout);
    exit(3);
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s -c <1|2|3|4> [-t threads] [-d seconds]\n", prog);
    exit(2);
}

int main(int argc, char **argv)
{
    int opt;
    char *end;
    while ((opt = getopt(argc, argv, "c:t:d:")) != -1) {
        switch (opt) {
        case 'c': ncase = (int)strtol(optarg, &end, 10); if (*end) usage(argv[0]); break;
        case 't': nthreads = (int)strtol(optarg, &end, 10); if (*end) usage(argv[0]); break;
        case 'd': seconds = strtod(optarg, &end); if (*end) usage(argv[0]); break;
        default: usage(argv[0]);
        }
    }
    if (ncase < 1 || ncase > 4 || nthreads < 1 || nthreads > MAX_WORKERS || !(seconds > 0))
        usage(argv[0]);

    static const char *names[] = { "", "sink", "bus", "catalog", "config" };
    void (*check)(void) = NULL;
    switch (ncase) {
    case 1: sink_init();    request = req_sink;    check = check_sink;    break;
    case 2: setup_bus();    request = req_bus;     check = check_bus;     break;
    case 3: catalog_init(); request = req_catalog; check = check_catalog; break;
    case 4: config_init();  request = req_config;  check = check_config;  break;
    }
    const char *k = getenv("DEVSVC_RWLOCK");
    printf("devsvc case %d (%s): threads=%d seconds=%g%s\n", ncase, names[ncase], nthreads, seconds,
           ncase == 3 && k && strcmp(k, "writer") == 0 ? " rwlock=writer-preferring" : "");
    fflush(stdout);

    for (int i = 0; i < nthreads; i++) {
        workers[i].id = i;
        workers[i].rng = 0x9e3779b97f4a7c15ul * (unsigned long)(i + 1);
        if (pthread_create(&workers[i].th, NULL, worker_main, &workers[i]) != 0) {
            perror("pthread_create");
            return 2;
        }
    }

    /* Watch the workers until all of them have finished. */
    double t0 = now_s(), t_stop = 0;
    long last[MAX_WORKERS];
    double last_t[MAX_WORKERS];
    for (int i = 0; i < nthreads; i++) {
        last[i] = -1;
        last_t[i] = t0;
    }
    long ops_at_stop = 0;
    for (;;) {
        usleep(20000);
        double t = now_s();
        int running = 0;
        for (int i = 0; i < nthreads; i++) {
            if (atomic_load(&workers[i].done))
                continue;
            running++;
            long o = atomic_load(&workers[i].ops);
            if (o != last[i]) {
                last[i] = o;
                last_t[i] = t;
            } else if (t - last_t[i] >= HANG_SECONDS) {
                report_hang(i, t - last_t[i]);
            }
        }
        if (!t_stop && t - t0 >= seconds) {
            for (int i = 0; i < nthreads; i++)
                ops_at_stop += atomic_load(&workers[i].ops);
            t_stop = t;
            atomic_store(&stop, 1);
        }
        if (t_stop && !running)
            break;
    }
    for (int i = 0; i < nthreads; i++)
        pthread_join(workers[i].th, NULL);

    long work = 0, overlap = 0;
    for (int i = 0; i < nthreads; i++) {
        work += workers[i].work;
        overlap += workers[i].overlap;
    }
    printf("ops=%ld ops_per_sec=%.0f parallel=%.0f%%\n", ops_at_stop, (double)ops_at_stop / (t_stop - t0),
           work ? 100.0 * (double)overlap / (double)work : 0.0);
    check();
    long nf = atomic_load(&n_fail);
    if (nf > 5)
        printf("check failed: ... %ld failures in all\n", nf);
    printf("result: %s\n", nf ? "FAIL" : "PASS");
    return nf ? 1 : 0;
}
