/* svcload: two clients of svc and one CPU hog, all SCHED_FIFO on CPU 0 (Level 2). Do not change.
 *
 * usage: sudo ./svcload [-d seconds] [-H hog_prio] [-o out.csv]
 *   cl-input   FIFO 30: from t=7 ms, every 50 ms one request of 2 ms, waits for the reply (a key press)
 *   cl-logger  FIFO 15: from t=6 ms, every 20 ms six requests of 1 ms at once, then waits for the six replies
 *   hog        FIFO 20 (-H, 0 = no hog): from t=3 ms, every 100 ms spins 30 ms. It never talks to svc.
 *   fixed timing: every other input request arrives while hog spins, and every other one right after a logger burst
 * prints response time (send -> reply) per client, how late each send was against its plan (send_late),
 * and the rt priority the worker reported.
 * uprobe anchors: req_send(id), req_done(id)
 */
#include "a5.h"
#include "svc_proto.h"
#include <sys/socket.h>
#include <sys/un.h>

#define MAXREQ 4096

struct rec {
    uint64_t id, t_plan, t_send, t_done; /* t_plan: when the client meant to send */
    int served_prio, queued;
};

static double secs = 10;
static int hog_prio = 20;
static uint64_t t0;
static volatile int done_flag;

__attribute__((noinline)) void req_send(uint64_t id) { __asm__ volatile("" : : "r"(id) : "memory"); }
__attribute__((noinline)) void req_done(uint64_t id) { __asm__ volatile("" : : "r"(id) : "memory"); }

static int connect_svc(void) {
    int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    memcpy(a.sun_path, SVC_ADDR, sizeof SVC_ADDR - 1);
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof SVC_ADDR - 1);
    if (s < 0 || connect(s, (struct sockaddr *)&a, alen)) {
        perror("svcload: connect (start ./svc first)");
        exit(1);
    }
    return s;
}

static void setup(const char *name, int prio) {
    pthread_setname_np(pthread_self(), name);
    if (pin_self(0) || set_fifo(prio)) {
        fprintf(stderr, "%s: SCHED_FIFO %d on CPU 0 failed (run with sudo)\n", name, prio);
        exit(1);
    }
}

struct client {
    const char *name;
    int id, prio, period_ms, burst, work_us, start_ms;
    struct rec r[MAXREQ];
    int n;
};

static struct client cls[2] = {
    { .name = "cl-input", .id = CL_INPUT, .prio = 30, .period_ms = 50, .burst = 1, .work_us = 2000, .start_ms = 7 },
    { .name = "cl-logger", .id = CL_LOGGER, .prio = 15, .period_ms = 20, .burst = 6, .work_us = 1000, .start_ms = 6 },
};

static void *client_main(void *arg) {
    struct client *c = arg;
    setup(c->name, c->prio);
    int s = connect_svc();
    uint64_t seq = 0, P = (uint64_t)c->period_ms * 1000000ull, end = t0 + (uint64_t)(secs * 1e9);
    for (uint64_t next = t0 + (uint64_t)c->start_ms * 1000000ull; next < end && c->n + c->burst <= MAXREQ; next += P) {
        sleep_until_ns(next);
        int first = c->n;
        for (int b = 0; b < c->burst; b++) {
            struct svc_req q = { .id = ((uint64_t)c->id << 32) | seq++, .client = c->id, .prio = c->prio,
                                 .work_us = (uint32_t)c->work_us };
            q.t_send = now_ns();
            c->r[c->n].id = q.id;
            c->r[c->n].t_send = q.t_send;
            c->r[c->n].t_plan = next;
            req_send(q.id);
            if (send(s, &q, sizeof q, 0) != (ssize_t)sizeof q) {
                perror("send");
                exit(1);
            }
            c->n++;
        }
        for (int b = first; b < c->n; b++) {
            struct svc_rep rep;
            if (recv(s, &rep, sizeof rep, 0) != (ssize_t)sizeof rep) {
                perror("recv");
                exit(1);
            }
            for (int k = first; k < c->n; k++) {
                if (c->r[k].id == rep.id) {
                    c->r[k].t_done = now_ns();
                    c->r[k].served_prio = rep.served_prio;
                    c->r[k].queued = rep.queued;
                    req_done(rep.id);
                }
            }
        }
    }
    close(s);
    return NULL;
}

static void *hog_main(void *arg) {
    (void)arg;
    setup("hog", hog_prio);
    uint64_t end = t0 + (uint64_t)(secs * 1e9);
    for (uint64_t next = t0 + 3000000ull; next < end; next += 100000000ull) {
        sleep_until_ns(next);
        burn_cpu(30000000ull); /* 30 ms of CPU, every 100 ms */
    }
    return NULL;
}

static void report(struct client *c, FILE *fo) {
    uint64_t v[MAXREQ], late[MAXREQ];
    int prios[100] = { 0 }, ok = 0;
    for (int i = 0; i < c->n; i++) {
        if (!c->r[i].t_done) {
            continue;
        }
        late[ok] = c->r[i].t_send > c->r[i].t_plan ? c->r[i].t_send - c->r[i].t_plan : 0;
        v[ok++] = c->r[i].t_done - c->r[i].t_send;
        if (c->r[i].served_prio >= 0 && c->r[i].served_prio < 100) {
            prios[c->r[i].served_prio]++;
        }
        if (fo) {
            fprintf(fo, "%s,%llu,%.1f,%.1f,%.1f,%d,%d\n", c->name, (unsigned long long)(c->r[i].id & 0xffffffffu),
                    (c->r[i].t_send - t0) / 1e3, late[ok - 1] / 1e3, (c->r[i].t_done - c->r[i].t_send) / 1e3,
                    c->r[i].served_prio, c->r[i].queued);
        }
    }
    uint64_t p50 = pct_u64(v, (size_t)ok, 50), p99 = pct_u64(v, (size_t)ok, 99), mx = ok ? v[ok - 1] : 0;
    uint64_t l99 = pct_u64(late, (size_t)ok, 99);
    printf("%-10s n=%4d  resp_ms p50=%6.2f p99=%6.2f max=%6.2f  send_late_p99_ms=%6.2f  served_prio:", c->name, ok,
           p50 / 1e6, p99 / 1e6, mx / 1e6, l99 / 1e6);
    for (int p = 0; p < 100; p++) {
        if (prios[p]) {
            printf(" %d x%d", p, prios[p]);
        }
    }
    printf("\n");
}

int main(int argc, char **argv) {
    const char *out = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "d:H:o:h")) != -1) {
        switch (opt) {
        case 'd': secs = atof(optarg); break;
        case 'H': hog_prio = atoi(optarg); break;
        case 'o': out = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-d seconds] [-H hog_prio|0] [-o out.csv]\n", argv[0]);
            return 1;
        }
    }
    t0 = now_ns() + 200000000ull;
    pthread_t th[3];
    int nth = 0;
    for (int i = 0; i < 2; i++) {
        pthread_create(&th[nth++], NULL, client_main, &cls[i]);
    }
    if (hog_prio > 0) {
        pthread_create(&th[nth++], NULL, hog_main, NULL);
    }
    printf("svcload pid=%d duration=%.0fs hog_prio=%d\n", (int)getpid(), secs, hog_prio);
    fflush(stdout);
    for (int i = 0; i < nth; i++) {
        pthread_join(th[i], NULL);
    }
    FILE *fo = out ? fopen(out, "w") : NULL;
    if (fo) {
        fprintf(fo, "client,seq,send_us,send_late_us,resp_us,served_prio,queued\n");
    }
    for (int i = 0; i < 2; i++) {
        report(&cls[i], fo);
    }
    if (fo) {
        fclose(fo);
        printf("per-request log: %s\n", out);
    }
    return 0;
}
