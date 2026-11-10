#include "common.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: cpu_busy SECONDS\n");
        return 2;
    }

    long seconds = number(argv[1], 1, 30);
    volatile uint64_t value = 1;
    ready();
    double start = now();

    /* KEY: runnable computation competes for CPU without growing its memory. */
    while (now() - start < seconds) {
        for (int i = 0; i < 10000; ++i) {
            value = value * 1664525 + 1013904223;
        }

    }

    printf("checksum=%llu seconds=%.6f\n", (unsigned long long)value, now() - start);
    return 0;
}
