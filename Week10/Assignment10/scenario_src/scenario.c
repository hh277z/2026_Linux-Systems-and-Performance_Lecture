#include "common.h"

/* Locally buildable workload. Inspecting this source is permitted. */
static void *entries[2048];

__attribute__((noinline)) static void *event_create(size_t bytes) {
    void *p = malloc(bytes);
    if (!p) {
        perror("event_create");
        exit(1);
    }

    lab_touch(p, bytes);
    return p;
}

__attribute__((noinline)) static void event_expire(size_t slot) {
    entries[slot] = NULL;
}

int main(int argc, char **argv) {
    if (argc < 2 || (!strcmp(argv[1], "--help"))) {
        puts("scenario M1|M2|M3 [--bytes 4MiB] [--base 8MiB] [--cycles 12] [--period 1] [--hold 1] "
             "[--seconds 60] [--max-bytes 256MiB] [--seed 1]");
        puts("M1-M3 use finite allocation/hold cycles; limits and service placement are configured "
             "by the launcher.");
        return argc < 2 ? 2 : 0;
    }

    if (strcmp(argv[1], "M1") && strcmp(argv[1], "M2") && strcmp(argv[1], "M3"))
        return 2;

    size_t bytes = 4UL << 20, base = 8UL << 20, cap = 256UL << 20;
    unsigned cycles = 12, seed = 1;
    double period = 1, hold = 1, seconds = 60;

    for (int i = 2; i < argc; i += 2) {
        if (i + 1 == argc)
            return 2;

        if (!strcmp(argv[i], "--bytes"))
            bytes = lab_size(argv[i + 1]);
        else if (!strcmp(argv[i], "--base"))
            base = lab_size(argv[i + 1]);
        else if (!strcmp(argv[i], "--max-bytes"))
            cap = lab_size(argv[i + 1]);
        else if (!strcmp(argv[i], "--cycles"))
            cycles = (unsigned)lab_number(argv[i + 1], 1, 2048);
        else if (!strcmp(argv[i], "--seed"))
            seed = (unsigned)lab_number(argv[i + 1], 1, UINT_MAX);
        else if (!strcmp(argv[i], "--period"))
            period = lab_number(argv[i + 1], .01, 3600);
        else if (!strcmp(argv[i], "--hold"))
            hold = lab_number(argv[i + 1], 0, 3600);
        else if (!strcmp(argv[i], "--seconds"))
            seconds = lab_number(argv[i + 1], .1, 3600);
        else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        }
    }

    if (!bytes || cap > (1UL << 30) || base > cap || bytes > cap - base ||
        (!strcmp(argv[1], "M1") && cycles > (cap - base) / bytes)) {
        fputs("allocation budget exceeded (hard ceiling 1GiB)\n", stderr);
        return 2;
    }

    lab_init();
    srand(seed);
    double start = lab_now(), deadline = start + seconds;
    void *baseline = malloc(base ? base : 1);

    if (!baseline)
        return 1;

    lab_touch(baseline, base);
    printf("t=%.6f event=start id=%s pid=%d page_size=%ld bytes=%zu base=%zu cycles=%u cap=%zu "
           "seed=%u\n",
           start, argv[1], getpid(), sysconf(_SC_PAGESIZE), bytes, base, cycles, cap, seed);

    for (unsigned i = 0; i < cycles && !lab_stop && lab_now() < deadline; i++) {
        if (!strcmp(argv[1], "M1")) {
            entries[i] = event_create(bytes);
            event_expire(i);
        } else if (!strcmp(argv[1], "M2")) {
            void *p = event_create(bytes);
            lab_sleep(hold);
            free(p);
        } else {
            void *p = event_create(bytes);
            double until = lab_now() + hold;

            while (!lab_stop && lab_now() < until && lab_now() < deadline) {
                lab_touch(p, bytes);
                lab_sleep(.01);
            }

            free(p);
        }

        printf("t=%.6f event=cycle id=%s cycle=%u\n", lab_now(), argv[1], i + 1);
        /* A safe allocator call also services a pending preload dump request. */
        void *beat = malloc(17);

        if (beat) {
            ((volatile char *)beat)[0] = 1;
            free(beat);
        }

        double remaining = deadline - lab_now();
        if (remaining > 0)
            lab_sleep(period < remaining ? period : remaining);
    }

    free(baseline);
    printf("t=%.6f event=end id=%s interrupted=%d\n", lab_now(), argv[1], lab_stop);
    return 0;
}
