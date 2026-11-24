/*
 * otawriter: writes one large update file, like an OTA install.
 *
 *   otawriter -f FILE -s size_mb [-c chunk_kb] [-r mb_per_s] [-y sync_every_mb] [-d]
 *
 *   -c  size of each write() call (default 1024 KB)
 *   -r  limit the write rate (0 = as fast as possible)
 *   -y  fsync every this many MB (0 = only at the end)
 *   -d  O_DIRECT: bypass the page cache
 *
 * The file is always fsynced at the end. The time printed includes that fsync,
 * so it is the time until the update is really on NAND.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull +
           (uint64_t)ts.tv_nsec;
}

static void die(const char *what)
{
    perror(what);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    long size_mb = 256, chunk_kb = 1024, sync_mb = 0;
    double rate = 0;
    int direct = 0, c;
    while ((c = getopt(argc, argv, "f:s:c:r:y:d")) != -1) {
        switch (c) {
        case 'f':
            file = optarg;
            break;
        case 's':
            size_mb = atol(optarg);
            break;
        case 'c':
            chunk_kb = atol(optarg);
            break;
        case 'r':
            rate = atof(optarg);
            break;
        case 'y':
            sync_mb = atol(optarg);
            break;
        case 'd':
            direct = 1;
            break;
        default:
            fprintf(stderr,
                    "usage: otawriter -f FILE -s size_mb [-c chunk_kb] "
                    "[-r mb_per_s] [-y sync_every_mb] [-d]\n");
            return 2;
        }
    }
    if (!file || size_mb < 1 || chunk_kb < 4) {
        fprintf(stderr, "otawriter: -f and -s are required\n");
        return 2;
    }

    int fd = open(file, O_WRONLY | O_CREAT | O_TRUNC | (direct ? O_DIRECT : 0), 0644);
    if (fd < 0)
        die(file);
    size_t chunk = (size_t)chunk_kb * 1024;
    char *buf;
    if (posix_memalign((void **)&buf, 4096, chunk))
        die("posix_memalign");
    for (size_t i = 0; i < chunk; i++) {
        buf[i] = (char)(i * 131 + 7);
    }

    uint64_t total = (uint64_t)size_mb << 20, done = 0, next_sync = (uint64_t)sync_mb << 20;
    uint64_t t0 = now_ns();
    while (done < total) {
        if (rate > 0) {
            /* Pace writes according to the configured rate. */
            uint64_t due = t0 + (uint64_t)((double)done / (rate * 1048576.0) * 1e9);
            struct timespec ts = {
                (time_t)(due / 1000000000ull),
                (long)(due % 1000000000ull)
            };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
        size_t n = total - done < chunk ? (size_t)(total - done) : chunk;
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            die("write");
        }
        done += (uint64_t)w;
        if (sync_mb && done >= next_sync) {
            if (fsync(fd) < 0)
                die("fsync");
            next_sync += (uint64_t)sync_mb << 20;
        }
    }
    uint64_t t_written = now_ns();
    if (fsync(fd) < 0)
        die("fsync");
    uint64_t t_end = now_ns();
    close(fd);
    double el = (double)(t_end - t0) / 1e9;
    printf("OTA result: throughput=%.1f MB/s, total=%.2f s, final_fsync=%.2f s\n",
           (double)size_mb / el, el, (double)(t_end - t_written) / 1e9);
    return 0;
}
