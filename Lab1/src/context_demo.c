#include "lab.h"

/* Compare waiting strategies over a fixed wall-time window, not equal work. */
int main(int argc, char **argv) {
    if (argc < 2 || argc > 4 || (strcmp(argv[1], "busy") && strcmp(argv[1], "sleep"))) {
        fprintf(stderr,
                "Usage: %s busy|sleep [duration_ms 20..3000] [sleep_ms 1..500]\n",
                argv[0]);
        return 2;
    }

    long duration = argc >= 3 ? number(argv[2], 20, 3000) : 1000;
    long interval = argc >= 4 ? number(argv[3], 1, 500) : 10;

    struct rusage before, after;
    if (getrusage(RUSAGE_SELF, &before)) {
        die("getrusage");
    }

    double start = seconds(), end = start + (double)duration / 1000.0;
    volatile uint64_t state = 1;
    unsigned long rounds = 0;
    double now;

    while ((now = seconds()) < end) {
        if (!strcmp(argv[1], "busy")) {
            for (int i = 0; i < 4096; ++i) {
                state = state * UINT64_C(6364136223846793005) + 1;
            }
        } else {
            long remaining = (long)((end - now) * 1000.0);
            long pause = remaining < interval ? remaining : interval;
            sleep_ms(pause > 0 ? pause : 1);
        }
        ++rounds;
    }

    double elapsed = (seconds() - start) * 1000.0;
    if (getrusage(RUSAGE_SELF, &after)) {
        die("getrusage");
    }
    double user = cpu_ms(after.ru_utime) - cpu_ms(before.ru_utime);
    double system = cpu_ms(after.ru_stime) - cpu_ms(before.ru_stime);

    printf("mode=%s elapsed_ms=%.3f user_ms=%.3f system_ms=%.3f\n", argv[1], elapsed,
           user, system);
    printf("voluntary=%ld nonvoluntary=%ld\n", after.ru_nvcsw - before.ru_nvcsw,
           after.ru_nivcsw - before.ru_nivcsw);
    printf("rounds=%lu checksum=%llu\n", rounds, (unsigned long long)state);
    puts("Counters cover this single process during the workload; rounds are not equal "
         "work.");

    return 0;
}
