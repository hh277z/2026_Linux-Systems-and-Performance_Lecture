#include "lab.h"

/* False sharing: two threads on two CPUs increment counters that share (0) or do not share (1) a cache line.
 * usage: fs_demo <seconds> <padded 0|1> [cpu_a] [cpu_b] */

struct adjacent {
    volatile uint64_t a;
    volatile uint64_t b; /* KEY same 64-byte line as a */
};

struct padded {
    _Alignas(64) volatile uint64_t a;
    _Alignas(64) volatile uint64_t b; /* KEY own cache line */
};

struct arg {
    volatile uint64_t *counter;
    int cpu;
    uint64_t ns, rounds;
};

static void *run(void *p) {
    struct arg *a = p;
    pin_cpu(a->cpu);
    uint64_t end = now_ns() + a->ns, r = 0;
    while (now_ns() < end) {
        for (int i = 0; i < 1024; i++) {
            (*a->counter)++; /* KEY every increment writes the line */
        }
        r++;
    }
    a->rounds = r * 1024;
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <seconds> <padded 0|1> [cpu_a] [cpu_b]\n", argv[0]);
        return 1;
    }
    int padded = atoi(argv[2]);
    int cpu_a = argc > 3 ? atoi(argv[3]) : 0, cpu_b = argc > 4 ? atoi(argv[4]) : 1;
    uint64_t ns = (uint64_t)(atof(argv[1]) * 1e9);
    static struct adjacent adj;
    static struct padded pad;
    struct arg a = { .counter = padded ? &pad.a : &adj.a, .cpu = cpu_a, .ns = ns };
    struct arg b = { .counter = padded ? &pad.b : &adj.b, .cpu = cpu_b, .ns = ns };
    printf("pid=%d padded=%d cpu_a=%d cpu_b=%d addr_a=%p addr_b=%p distance=%ld bytes\n", (int)getpid(), padded,
           cpu_a, cpu_b, (void *)a.counter, (void *)b.counter, (long)((char *)b.counter - (char *)a.counter));
    fflush(stdout);
    pthread_t ta, tb;
    if (pthread_create(&ta, NULL, run, &a) || pthread_create(&tb, NULL, run, &b)) {
        die("pthread_create");
    }
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    printf("increments: a=%llu b=%llu total/s=%.0f\n", (unsigned long long)a.rounds, (unsigned long long)b.rounds,
           (a.rounds + b.rounds) / (ns / 1e9));
    return 0;
}
