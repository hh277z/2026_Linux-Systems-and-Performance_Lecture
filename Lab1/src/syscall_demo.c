#include "lab.h"
#include <fcntl.h>

/* syscw is a kernel write-family counter, not a per-syscall histogram. */
static int syscw(unsigned long long *value) {
    FILE *f = fopen("/proc/self/io", "r");
    if (!f) {
        return 0;
    }
    char line[256];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "syscw: %llu", value) == 1) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void run(const char *mode, size_t total, int repeat) {
    char buffer[4096];
    memset(buffer, 'L', sizeof buffer);

    int use_stdio = !strcmp(mode, "stdio");
    size_t chunk = !strcmp(mode, "byte") ? 1 : sizeof buffer;
    unsigned long long calls = 0, attempts = 0, c0 = 0, c1 = 0;
    FILE *stream = NULL;
    int fd = -1;

    if (use_stdio) {
        stream = fopen("/dev/null", "w");
        if (!stream) {
            die("fopen /dev/null");
        }
        if (setvbuf(stream, buffer, _IOFBF, sizeof buffer)) {
            fputs("setvbuf failed\n", stderr);
            exit(1);
        }
    } else {
        fd = open("/dev/null", O_WRONLY);
        if (fd < 0) {
            die("open /dev/null");
        }
    }

    fflush(stdout); /* Keep summary output out of the measured counter window. */
    int have_before = syscw(&c0);
    struct rusage before, after;
    if (getrusage(RUSAGE_SELF, &before)) {
        die("getrusage");
    }

    double start = seconds();

    if (use_stdio) {
        for (size_t i = 0; i < total; ++i) {
            ++calls;
            if (fputc('L', stream) == EOF) {
                die("fputc");
            }
        }
        if (fflush(stream)) {
            die("fflush"); /* Include the final buffered write. */
        }
    } else {
        size_t done = 0;
        while (done < total) {
            size_t wanted = total - done < chunk ? total - done : chunk;
            ++calls;
            size_t sent = 0;
            while (sent < wanted) {
                ++attempts; /* Includes EINTR retries and short-write retries. */
                ssize_t n = write(fd, buffer + sent, wanted - sent);
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                if (n <= 0) {
                    if (n == 0) {
                        errno = EIO;
                    }
                    die("write");
                }
                sent += (size_t)n;
            }
            done += sent;
        }
    }

    double elapsed = (seconds() - start) * 1000.0;
    if (getrusage(RUSAGE_SELF, &after)) {
        die("getrusage");
    }
    int have_after = syscw(&c1);
    if (stream) {
        if (fclose(stream)) {
            die("fclose");
        }
    } else if (close(fd)) {
        die("close");
    }

    char attempts_text[32];
    char syscw_text[32];

    if (use_stdio) {
        snprintf(attempts_text, sizeof attempts_text, "NA");
    } else {
        snprintf(attempts_text, sizeof attempts_text, "%llu", attempts);
    }

    if (have_before && have_after && c1 >= c0) {
        snprintf(syscw_text, sizeof syscw_text, "%llu", c1 - c0);
    } else {
        snprintf(syscw_text, sizeof syscw_text, "NA");
    }

    printf("%-6s\t%6d\t%10zu\t%12llu\t%14s\t%11s\t%10.3f\t%9.3f\t%9.3f\n", mode, repeat,
           total, calls, attempts_text, syscw_text, elapsed,
           cpu_ms(after.ru_utime) - cpu_ms(before.ru_utime),
           cpu_ms(after.ru_stime) - cpu_ms(before.ru_stime));
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4 ||
        (strcmp(argv[1], "compare") && strcmp(argv[1], "byte") &&
         strcmp(argv[1], "batch") && strcmp(argv[1], "stdio"))) {
        fprintf(
            stderr,
            "Usage: %s compare|byte|batch|stdio [bytes 1..8388608] [repeats 1..5]\n",
            argv[0]);
        return 2;
    }

    size_t total = argc >= 3 ? (size_t)number(argv[2], 1, 8388608) : 1048576;
    int repeats = argc >= 4 ? (int)number(argv[3], 1, 5) : 3;
    const char *modes[] = {"byte", "batch", "stdio"};

    printf("%-6s\t%6s\t%10s\t%12s\t%14s\t%11s\t%10s\t%9s\t%9s\n", "mode", "repeat",
           "bytes", "app_calls", "write_attempts", "syscw_delta", "elapsed_ms",
           "user_ms", "system_ms");

    for (int r = 0; r < repeats; ++r) {
        if (!strcmp(argv[1], "compare")) {
            /* Rotate the order to reduce a fixed warmup/order bias. */
            for (int i = 0; i < 3; ++i) {
                run(modes[(r + i) % 3], total, r + 1);
            }
        } else {
            run(argv[1], total, r + 1);
        }
    }

    return 0;
}
