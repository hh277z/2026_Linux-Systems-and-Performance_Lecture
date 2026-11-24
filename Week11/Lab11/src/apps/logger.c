/*
 * logger: writes log lines the way a TV service would (lecture p.52).
 *
 *   logger -f FILE [-m line|batch|nosync] [-n lines] [-s line_bytes] [-r lines_per_s]
 *          [-b batch_kb] [-t flush_ms] [-y fsync|fdatasync] [-a ackfile] [-w writefile] [-A]
 *   logger -f FILE -P                 create an empty file and make its name durable
 *   logger -c FILE -a ackfile [-w writefile]  check a log file after a power cut
 *
 * Modes
 *   line    write() and fsync() every line (logger A on p.52)
 *   batch   keep lines in RAM, write and fsync when batch_kb is full or flush_ms
 *           has passed since the oldest unsynced line (logger B on p.52)
 *   nosync  write() every line and never sync: the page cache decides
 *
 * Each line starts with its sequence number. After every sync the logger
 * records the last synced sequence number in ackfile (keep it outside the
 * eMMC, e.g. in logs/). If -w is given, the logger also records the last
 * complete write_all() operation there on exit or I/O error. After a power
 * cut, -c compares those host-side observations with the recovered file.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *write_progress_file;
static uint64_t last_write_seq;
static int save_write_progress(void);

static void die(const char *what)
{
    int saved_errno = errno;
    if (write_progress_file && save_write_progress() < 0)
        fprintf(stderr, "could not save write progress: %s\n", strerror(errno));
    errno = saved_errno;
    perror(what);
    exit(1);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: logger -f FILE [-m line|batch|nosync] [-n lines] "
            "[-s line_bytes] [-r lines_per_s]\n"
            "              [-b batch_kb] [-t flush_ms] [-y fsync|fdatasync] "
            "[-a ackfile] [-w writefile] [-A]\n"
            "       logger -f FILE -P\n"
            "       logger -c FILE -a ackfile [-w writefile]\n");
    exit(2);
}

/* ---------- sync latency samples ---------- */
static uint64_t *lat;
static size_t nlat, caplat;

static void add_lat(uint64_t ns)
{
    if (nlat == caplat) {
        caplat = caplat ? caplat * 2 : 1024;
        lat = realloc(lat, caplat * sizeof *lat);
        if (!lat)
            die("realloc");
    }
    lat[nlat++] = ns;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t pct(double p)
{
    if (!nlat)
        return 0;
    size_t i = (size_t)(p / 100.0 * (double)(nlat - 1) + 0.5);
    return lat[i];
}

/* ---------- writing ---------- */
static int fd, ackfd = -1, use_fdatasync;
static uint64_t syncs, app_bytes;

/* Host-side observation, deliberately written only at start and exit/error,
 * never once per application line (which would perturb the workload). */
static int save_write_progress(void)
{
    if (!write_progress_file)
        return 0;
    int pfd = open(write_progress_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pfd < 0)
        return -1;
    char rec[32];
    int n = snprintf(rec, sizeof rec, "%020lu\n", (unsigned long)last_write_seq);
    ssize_t done = 0;
    while (done < n) {
        ssize_t w = write(pfd, rec + done, (size_t)(n - done));
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(pfd);
            return -1;
        }
        done += w;
    }
    if (fsync(pfd) < 0) {
        close(pfd);
        return -1;
    }
    return close(pfd);
}

static int prepare_file(const char *path)
{
    int pfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (pfd < 0)
        return -1;
    if (fsync(pfd) < 0) {
        close(pfd);
        return -1;
    }
    if (close(pfd) < 0)
        return -1;

    char *copy = strdup(path);
    if (!copy)
        return -1;
    char *slash = strrchr(copy, '/');
    const char *dir = ".";
    if (slash) {
        if (slash == copy) {
            slash[1] = '\0';
            dir = copy;
        } else {
            *slash = '\0';
            dir = copy;
        }
    }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) {
        free(copy);
        return -1;
    }
    int rc = fsync(dfd);
    int saved_errno = errno;
    close(dfd);
    free(copy);
    errno = saved_errno;
    return rc;
}

static void write_all(const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            die("write");
        }
        p += w;
        n -= (size_t)w;
    }
}

static void sync_now(uint64_t last_seq)
{
    uint64_t t0 = now_ns();
    if ((use_fdatasync ? fdatasync(fd) : fsync(fd)) < 0)
        die("fsync");
    add_lat(now_ns() - t0);
    syncs++;
    if (ackfd >= 0) {
        /* The line is durable; tell the checker. */
        char rec[32];
        int n = snprintf(rec, sizeof rec, "%020lu\n", (unsigned long)last_seq);
        if (pwrite(ackfd, rec, (size_t)n, 0) != n)
            die("ackfile");
    }
}

static int make_line(char *buf, size_t size, uint64_t seq, double t)
{
    int n = snprintf(buf, size,
                     "%08lu %011.3f service=epg level=info msg=",
                     (unsigned long)seq, t);
    for (; (size_t)n < size - 1; n++) {
        buf[n] = (char)('a' + (seq + (uint64_t)n) % 26);
    }
    buf[size - 1] = '\n';
    return (int)size;
}

/* ---------- checking after a power cut ---------- */
static int read_progress(const char *path, unsigned long *value)
{
    if (!path)
        return 0;
    FILE *p = fopen(path, "r");
    if (!p)
        return 0;
    int ok = fscanf(p, "%lu", value) == 1;
    fclose(p);
    return ok ? 1 : -1;
}

static int check(const char *file, const char *ackfile, const char *writefile)
{
    unsigned long acked = 0;
    unsigned long seq, expect = 1, last = 0, bad = 0;
    unsigned long write_completed = 0;
    int have_write_progress = read_progress(writefile, &write_completed);
    FILE *a = fopen(ackfile, "r");
    if (a) {
        if (fscanf(a, "%lu", &acked) != 1)
            acked = 0;
        fclose(a);
    }
    FILE *f = fopen(file, "r");
    if (!f) {
        /* The file's creation itself never reached NAND. */
        printf("recovery_file=missing\n");
    }
    char line[8192];
    while (f && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lu", &seq) != 1 || seq != expect) {
            bad++;
            break;
        }
        last = seq;
        expect++;
    }
    if (f)
        fclose(f);
    if (have_write_progress > 0)
        printf("write_completed_line=%lu\n", write_completed);
    else
        printf("write_completed_line=N/A\n");
    printf("fsync_ack_line=%lu\n", acked);
    printf("recovered_contiguous_line=%lu\n", last);
    if (bad)
        printf("recovery_sequence=non_contiguous\n");
    if (!acked) {
        printf("result=NO_FSYNC_ACK\n");
        return 0;
    }
    if (last >= acked) {
        printf("result=PASS\n");
        return 0;
    }
    printf("result=FAIL; lost_fsync_acknowledged_lines=%lu\n", acked - last);
    return 1;
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *mode = "line";
    const char *ackfile = NULL;
    const char *checkfile = NULL;
    const char *writefile = NULL;
    long lines = 2000;
    long line_bytes = 200;
    long rate = 0;
    long batch_kb = 64;
    long flush_ms = 0;
    int c, prepare = 0, append = 0;
    while ((c = getopt(argc, argv, "f:m:n:s:r:b:t:y:a:c:w:PA")) != -1) {
        switch (c) {
        case 'f':
            file = optarg;
            break;
        case 'm':
            mode = optarg;
            break;
        case 'n':
            lines = atol(optarg);
            break;
        case 's':
            line_bytes = atol(optarg);
            break;
        case 'r':
            rate = atol(optarg);
            break;
        case 'b':
            batch_kb = atol(optarg);
            break;
        case 't':
            flush_ms = atol(optarg);
            break;
        case 'y':
            use_fdatasync = strcmp(optarg, "fdatasync") == 0;
            break;
        case 'a':
            ackfile = optarg;
            break;
        case 'c':
            checkfile = optarg;
            break;
        case 'w':
            writefile = optarg;
            break;
        case 'P':
            prepare = 1;
            break;
        case 'A':
            append = 1;
            break;
        default: usage();
        }
    }
    if (prepare) {
        if (!file || checkfile)
            usage();
        if (prepare_file(file) < 0)
            die("prepare file");
        return 0;
    }
    if (checkfile) {
        if (!ackfile)
            usage();
        return check(checkfile, ackfile, writefile);
    }
    if (!file || line_bytes < 40 || line_bytes > 4096 || lines < 1)
        usage();
    int m_line = strcmp(mode, "line") == 0;
    int m_batch = strcmp(mode, "batch") == 0;
    int m_nosync = strcmp(mode, "nosync") == 0;
    if (!m_line && !m_batch && !m_nosync)
        usage();

    write_progress_file = writefile;
    fd = open(file, O_WRONLY | O_CREAT | O_APPEND | (append ? 0 : O_TRUNC), 0644);
    if (fd < 0)
        die(file);
    if (write_progress_file && save_write_progress() < 0)
        die("write progress");
    if (ackfile &&
        (ackfd = open(ackfile, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
        die(ackfile);
    }

    size_t cap = (size_t)batch_kb * 1024 + (size_t)line_bytes;
    char *buf = malloc(cap), *line = malloc((size_t)line_bytes);
    size_t used = 0;
    uint64_t t_start = now_ns(), t_oldest = 0;

    for (long i = 1; i <= lines; i++) {
        if (rate > 0) {
            /* Keep the requested line rate. */
            uint64_t due = t_start + (uint64_t)(i - 1) * 1000000000ull / (uint64_t)rate;
            struct timespec ts = {
                (time_t)(due / 1000000000ull),
                (long)(due % 1000000000ull)
            };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
        uint64_t t = now_ns();
        int n = make_line(line, (size_t)line_bytes, (uint64_t)i, (double)(t - t_start) / 1e9);
        app_bytes += (uint64_t)n;
        if (m_line) {
            write_all(line, (size_t)n);
            last_write_seq = (uint64_t)i;
            sync_now((uint64_t)i);
        } else if (m_nosync) {
            write_all(line, (size_t)n);
            last_write_seq = (uint64_t)i;
        } else {
            if (!used)
                t_oldest = t;
            memcpy(buf + used, line, (size_t)n);
            used += (size_t)n;
            int full = used >= (size_t)batch_kb * 1024;
            int old = flush_ms > 0 && t - t_oldest >= (uint64_t)flush_ms * 1000000ull;
            if (full || old || i == lines) {
                write_all(buf, used);
                last_write_seq = (uint64_t)i;
                sync_now((uint64_t)i);
                used = 0;
            }
        }
    }
    double el = (double)(now_ns() - t_start) / 1e9;
    qsort(lat, nlat, sizeof *lat, cmp_u64);
    printf("logger result: completed_lines=%ld, written_bytes=%lu, "
           "elapsed_s=%.3f, sync_calls=%lu\n",
           lines, (unsigned long)app_bytes, el, (unsigned long)syncs);
    if (nlat) {
        printf("sync latency: p50 %.0f us  p99 %.0f us  max %.0f us  (%s)\n",
               pct(50) / 1e3, pct(99) / 1e3, lat[nlat - 1] / 1e3,
               use_fdatasync ? "fdatasync" : "fsync");
    }
    if (write_progress_file && save_write_progress() < 0)
        die("write progress");
    close(fd);
    return 0;
}
