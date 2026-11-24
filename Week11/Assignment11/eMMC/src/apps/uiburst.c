/*
 * uiburst: UI reads that come in bursts, like an app launch (lecture p.9:
 * hundreds of small random reads, QD 1-4).
 *
 *   uiburst -f FILE [-n reads_per_burst] [-p period_ms] [-d seconds | -w pidfile] [-o out.csv]
 *
 * Every period_ms, read n random 4 KB blocks of FILE with O_DIRECT, one after
 * another (QD 1). The time of one burst is what a user waits for the app to open.
 * Stops after -d seconds, or when the process in -w pidfile has exited.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t pct(uint64_t *v, size_t n, double p)
{
    return n ? v[(size_t)(p / 100.0 * (double)(n - 1) + 0.5)] : 0;
}

static volatile sig_atomic_t stop;
static void on_sig(int s)
{
    (void)s;
    stop = 1;
}

int main(int argc, char **argv)
{
    const char *file = NULL, *out = NULL, *waitpidf = NULL;
    long n = 200, period_ms = 500, seconds = 0;
    int c;
    while ((c = getopt(argc, argv, "f:n:p:d:w:o:")) != -1) {
        switch (c) {
        case 'f':
            file = optarg;
            break;
        case 'n':
            n = atol(optarg);
            break;
        case 'p':
            period_ms = atol(optarg);
            break;
        case 'd':
            seconds = atol(optarg);
            break;
        case 'w':
            waitpidf = optarg;
            break;
        case 'o':
            out = optarg;
            break;
        default:
            fprintf(stderr,
                    "usage: uiburst -f FILE [-n reads_per_burst] [-p period_ms] "
                    "[-d seconds | -w pidfile] [-o out.csv]\n");
            return 2;
        }
    }
    if (!file || n < 1 || period_ms < 1 || (!seconds && !waitpidf)) {
        fprintf(stderr, "uiburst: -f and one of -d, -w are required\n");
        return 2;
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    int fd = open(file, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        perror(file);
        return 1;
    }
    struct stat sb;
    fstat(fd, &sb);
    uint64_t blocks = (uint64_t)sb.st_size / 4096;
    if (!blocks) {
        fprintf(stderr, "uiburst: %s is empty\n", file);
        return 1;
    }
    char *buf;
    if (posix_memalign((void **)&buf, 4096, 4096)) {
        return 1;
    }
    FILE *csv = out ? fopen(out, "w") : NULL;
    if (csv) {
        fprintf(csv, "burst,start_ms,burst_us,max_read_us\n");
    }

    size_t cap = 1024, nb = 0, nr = 0, capr = 65536;
    uint64_t *burst = malloc(cap * sizeof *burst), *rd = malloc(capr * sizeof *rd);
    uint64_t t0 = now_ns(), rng = 88172645463325252ull;
    pid_t watch = 0;
    if (waitpidf) {
        for (int i = 0; i < 200 && !watch; i++) {     /* the writer may not have started yet */
            FILE *pf = fopen(waitpidf, "r");
            if (pf) {
                if (fscanf(pf, "%d", &watch) != 1) {
                    watch = 0;
                }
                fclose(pf);
            }
            if (!watch) {
                usleep(10000);
            }
        }
    }
    for (uint64_t k = 0; !stop; k++) {
        uint64_t start = t0 + k * (uint64_t)period_ms * 1000000ull;
        if (seconds && start >= t0 + (uint64_t)seconds * 1000000000ull) {
            break;
        }
        if (watch && kill(watch, 0) < 0 && errno == ESRCH) {
            break;
        }
        struct timespec ts = {
            (time_t)(start / 1000000000ull),
            (long)(start % 1000000000ull),
        };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        uint64_t b0 = now_ns(), worst = 0;
        for (long i = 0; i < n && !stop; i++) {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            uint64_t t = now_ns();
            if (pread(fd, buf, 4096, (off_t)((rng % blocks) * 4096)) != 4096) {
                perror("pread");
                return 1;
            }
            t = now_ns() - t;
            if (t > worst) {
                worst = t;
            }
            if (nr == capr) {
                capr *= 2;
                rd = realloc(rd, capr * sizeof *rd);
            }
            rd[nr++] = t;
        }
        uint64_t b = now_ns() - b0;
        if (nb == cap) {
            cap *= 2;
            burst = realloc(burst, cap * sizeof *burst);
        }
        burst[nb++] = b;
        if (csv) {
            fprintf(csv, "%lu,%.1f,%.0f,%.0f\n", (unsigned long)k,
                    (b0 - t0) / 1e6, b / 1e3, worst / 1e3);
        }
    }
    if (csv) {
        fclose(csv);
    }
    qsort(burst, nb, sizeof *burst, cmp_u64);
    qsort(rd, nr, sizeof *rd, cmp_u64);
    printf("uiburst: %lu bursts of %ld reads  burst p50 %.1f ms  p90 %.1f ms  "
           "max %.1f ms  |  read p50 %.0f us  p99 %.0f us\n",
           (unsigned long)nb, n, pct(burst, nb, 50) / 1e6, pct(burst, nb, 90) / 1e6,
           nb ? burst[nb - 1] / 1e6 : 0, pct(rd, nr, 50) / 1e3, pct(rd, nr, 99) / 1e3);
    return 0;
}
