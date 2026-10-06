#include "lab.h"

/* Working-set sweep: random pointer chase over buffers of growing size, ns per access.
 * usage: ws_demo [min_kb] [max_kb] [accesses_per_size] */

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next_rand(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

/* Build one random cycle through all cache-line-sized slots (Sattolo's algorithm). */
static void build_cycle(uint32_t *next, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        next[i] = i;
    }
    for (uint32_t i = n - 1; i > 0; i--) {
        uint32_t j = (uint32_t)(next_rand() % i);
        uint32_t t = next[i];
        next[i] = next[j];
        next[j] = t;
    }
}

int main(int argc, char **argv) {
    long min_kb = argc > 1 ? atol(argv[1]) : 16, max_kb = argc > 2 ? atol(argv[2]) : 65536;
    long accesses = argc > 3 ? atol(argv[3]) : 20000000;
    printf("pid=%d  size_kb  ns_per_access   (random pointer chase, 64-byte stride)\n", (int)getpid());
    for (long kb = min_kb; kb <= max_kb; kb *= 2) {
        uint32_t n = (uint32_t)(kb * 1024 / 64);
        char *buf = aligned_alloc(64, (size_t)n * 64);
        uint32_t *next = malloc((size_t)n * sizeof *next);
        if (!buf || !next) {
            die("alloc");
        }
        build_cycle(next, n);
        for (uint32_t i = 0; i < n; i++) {
            *(uint32_t *)(buf + (size_t)i * 64) = next[i]; /* KEY each line stores the index of the next line */
        }
        uint32_t p = 0;
        uint64_t t0 = now_ns();
        for (long i = 0; i < accesses; i++) {
            p = *(uint32_t *)(buf + (size_t)p * 64); /* KEY dependent load: latency, not bandwidth */
        }
        uint64_t t1 = now_ns();
        printf("%8ld  %8.2f%s\n", kb, (double)(t1 - t0) / accesses, p == 0xFFFFFFFF ? " " : "");
        fflush(stdout);
        free(buf);
        free(next);
    }
    return 0;
}
