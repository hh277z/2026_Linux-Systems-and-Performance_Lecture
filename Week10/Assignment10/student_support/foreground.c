#include "../scenario_src/common.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        fputs("foreground BYTES SECONDS\n", stderr);
        return 2;
    }

    size_t n = lab_size(argv[1]);
    double seconds = lab_number(argv[2], .1, 600);
    if (!n || n > (1UL << 30))
        return 2;

    lab_init();
    void *p = malloc(n);

    if (!p)
        return 1;

    lab_touch(p, n);
    double end = lab_now() + seconds;
    unsigned long count = 0;

    while (!lab_stop && lab_now() < end) {
        double t = lab_now();
        lab_touch(p, n);
        printf("t=%.6f event=frame latency_us=%.3f frame=%lu\n", lab_now(), (lab_now() - t) * 1e6,
               ++count);
        lab_sleep(.02);
    }

    free(p);
    return 0;
}
