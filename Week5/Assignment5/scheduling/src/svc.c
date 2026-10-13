/* svc: a request/reply service on a unix socket, with optional manual priority inheritance (Level 2).
 *
 * usage: sudo ./svc [-i] [-q fifo|prio] [-b base_prio] [-R rx_prio] [-c cpu] [-d seconds]
 *   -i       inherit: the worker serves at max(base, highest priority waiting in the queue)
 *   -q       queue order: fifo (arrival order, default) or prio (highest client priority first)
 *   -b       worker's own SCHED_FIFO priority (default 10)
 *   -R       receiver thread's SCHED_FIFO priority (default 50): it only reads and queues requests
 *   -d       exit after this many seconds (default 0 = until Ctrl-C)
 *
 * threads (both pinned to -c, default CPU 0):
 *   svc-rx      accept + recv, put requests in the queue, wake the worker
 *   svc-worker  take a request, burn work_us of CPU, reply
 * uprobe anchors: req_enqueue(id), handle_begin(id, prio), handle_end(id)
 */
#include "a5.h"
#include "svc_proto.h"
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>

#define QMAX 256
#define MAXCL 16

struct item {
    struct svc_req r;
    int fd;
    uint64_t seq;
};

static struct item q[QMAX];
static int qn;
static uint64_t qseq;
static pthread_mutex_t qlock;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;
static int inherit, by_prio, base_prio = 10, rx_prio = 50, cpu;
static pthread_t worker_th;
static int worker_prio;          /* priority the worker currently runs at (protected by qlock) */
static volatile int stop;
static uint64_t served, boosts;

__attribute__((noinline)) void req_enqueue(uint64_t id) { __asm__ volatile("" : : "r"(id) : "memory"); }
__attribute__((noinline)) void handle_begin(uint64_t id, int prio) { __asm__ volatile("" : : "r"(id), "r"(prio) : "memory"); }
__attribute__((noinline)) void handle_end(uint64_t id) { __asm__ volatile("" : : "r"(id) : "memory"); }

static int queue_max_prio(void) {
    int m = 0;
    for (int i = 0; i < qn; i++) {
        m = q[i].r.prio > m ? q[i].r.prio : m;
    }
    return m;
}

/* Set the worker's priority. Called with qlock held, by either thread. */
static void set_worker_prio(int p) {
    if (p == worker_prio) {
        return;
    }
    struct sched_param sp = { .sched_priority = p };
    int rc = pthread_setschedparam(worker_th, SCHED_FIFO, &sp); /* KEY change another thread's priority */
    if (rc) {
        fprintf(stderr, "svc: setschedparam(worker, %d): %s\n", p, strerror(rc));
        return;
    }
    if (p > worker_prio) {
        boosts++;
    }
    worker_prio = p;
}

/* Pick the next request: index into q[]. Called with qlock held, qn > 0. */
static int pick(void) {
    int best = 0;
    for (int i = 1; i < qn; i++) {
        if (by_prio) {
            if (q[i].r.prio > q[best].r.prio || (q[i].r.prio == q[best].r.prio && q[i].seq < q[best].seq)) {
                best = i; /* KEY prio queue: highest client priority first, arrival order among equals */
            }
        } else if (q[i].seq < q[best].seq) {
            best = i;
        }
    }
    return best;
}

static void *worker(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "svc-worker");
    for (;;) {
        pthread_mutex_lock(&qlock);
        while (qn == 0 && !stop) {
            if (inherit) {
                set_worker_prio(base_prio); /* KEY nothing waiting: back to the worker's own priority */
            }
            pthread_cond_wait(&qcond, &qlock);
        }
        if (stop && qn == 0) {
            pthread_mutex_unlock(&qlock);
            return NULL;
        }
        int i = pick();
        struct item it = q[i];
        q[i] = q[--qn];
        int queued = qn;
        if (inherit) {
            int want = it.r.prio > queue_max_prio() ? it.r.prio : queue_max_prio();
            set_worker_prio(want > base_prio ? want : base_prio); /* KEY serve at the highest waiting priority */
        }
        int prio_now = worker_prio;
        pthread_mutex_unlock(&qlock);

        struct svc_rep rep = { .id = it.r.id, .served_prio = prio_now, .queued = queued };
        rep.t_begin = now_ns();
        handle_begin(it.r.id, prio_now);
        burn_cpu((uint64_t)it.r.work_us * 1000);
        handle_end(it.r.id);
        rep.t_end = now_ns();
        if (send(it.fd, &rep, sizeof rep, MSG_NOSIGNAL) != (ssize_t)sizeof rep) {
            /* client went away */
        }
        __atomic_add_fetch(&served, 1, __ATOMIC_RELAXED);
    }
}

static void on_int(int s) {
    (void)s;
    stop = 1;
}

int main(int argc, char **argv) {
    int opt;
    double secs = 0;
    while ((opt = getopt(argc, argv, "iq:b:R:c:d:h")) != -1) {
        switch (opt) {
        case 'i': inherit = 1; break;
        case 'q': by_prio = !strcmp(optarg, "prio"); break;
        case 'b': base_prio = atoi(optarg); break;
        case 'R': rx_prio = atoi(optarg); break;
        case 'c': cpu = atoi(optarg); break;
        case 'd': secs = atof(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-i] [-q fifo|prio] [-b base_prio] [-R rx_prio] [-c cpu] [-d seconds]\n", argv[0]);
            return 1;
        }
    }
    signal(SIGINT, on_int);
    signal(SIGTERM, on_int);
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT); /* KEY queue lock protocol: priority inheritance (explain why in explanation 9) */
    pthread_mutex_init(&qlock, &ma);

    int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, SVC_ADDR, sizeof SVC_ADDR - 1);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof SVC_ADDR - 1);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, alen) || listen(ls, 16)) {
        perror("svc: socket/bind (is another svc running?)");
        return 1;
    }

    /* worker first, so the receiver can change its priority */
    if (pthread_create(&worker_th, NULL, worker, NULL)) {
        perror("pthread_create");
        return 1;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    struct sched_param sp = { .sched_priority = base_prio };
    worker_prio = base_prio;
    if (pthread_setaffinity_np(worker_th, sizeof set, &set) || pthread_setschedparam(worker_th, SCHED_FIFO, &sp)) {
        fprintf(stderr, "svc: worker setup failed (run with sudo)\n");
        return 1;
    }
    pthread_setname_np(pthread_self(), "svc-rx");
    if (pin_self(cpu) || set_fifo(rx_prio)) {
        fprintf(stderr, "svc: rx setup failed (run with sudo)\n");
        return 1;
    }
    printf("svc pid=%d inherit=%d queue=%s base=%d rx=%d cpu=%d\n", (int)getpid(), inherit, by_prio ? "prio" : "fifo",
           base_prio, rx_prio, cpu);
    fflush(stdout);

    struct pollfd pf[MAXCL + 1];
    int nf = 1;
    pf[0].fd = ls;
    pf[0].events = POLLIN;
    uint64_t t_end = secs > 0 ? now_ns() + (uint64_t)(secs * 1e9) : 0;
    while (!stop && (!t_end || now_ns() < t_end)) {
        int n = poll(pf, (nfds_t)nf, 200);
        if (n <= 0) {
            continue;
        }
        if ((pf[0].revents & POLLIN) && nf <= MAXCL) {
            int c = accept(ls, NULL, NULL);
            if (c >= 0) {
                pf[nf].fd = c;
                pf[nf].events = POLLIN;
                nf++;
            }
        }
        for (int k = 1; k < nf; k++) {
            if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            struct svc_req r;
            ssize_t got = recv(pf[k].fd, &r, sizeof r, MSG_DONTWAIT);
            if (got == 0 || (got < 0 && errno != EAGAIN)) {
                close(pf[k].fd);
                pf[k] = pf[--nf];
                k--;
                continue;
            }
            if (got != (ssize_t)sizeof r) {
                continue;
            }
            pthread_mutex_lock(&qlock);
            if (qn < QMAX) {
                q[qn].r = r;
                q[qn].fd = pf[k].fd;
                q[qn].seq = qseq++;
                qn++;
                req_enqueue(r.id);
                if (inherit && r.prio > worker_prio) {
                    set_worker_prio(r.prio); /* KEY -i: raise the worker before waking it (explain why in explanation 7) */
                }
                pthread_cond_signal(&qcond);
            }
            pthread_mutex_unlock(&qlock);
        }
    }
    pthread_mutex_lock(&qlock);
    stop = 1;
    pthread_cond_broadcast(&qcond);
    pthread_mutex_unlock(&qlock);
    pthread_join(worker_th, NULL);
    printf("svc served=%llu boosts=%llu\n", (unsigned long long)served, (unsigned long long)boosts);
    return 0;
}
