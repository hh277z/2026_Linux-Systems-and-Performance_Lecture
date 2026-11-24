/*
 * logger: writes log lines the way a TV service would (lecture p.52).
 *
 *   logger -f FILE [-m line|batch|nosync|policy] [-n lines] [-s line_bytes] [-r lines_per_s]
 *          [-b buf_kb] [-k lines_per_sync] [-t max_delay_ms] [-e error_every] [-y
     fsync|fdatasync] [-a ackfile]
 *   logger -c FILE -a ackfile        check a log file after a power cut
 *
 * Modes
 *   line    write() and fsync() every line (logger A on p.52)
 *   batch   keep lines in RAM, write and fsync when buf_kb is full, every lines_per_sync
 *           lines (-k), or when max_delay_ms has passed since the oldest unsynced line (logger
     B on p.52)
 *   nosync  write() every line and never sync: the page cache decides
 *   policy  hand every line to src/apps/logpolicy.c (Assignment 11, Level 2)
 *
 * Every error_every-th line is an error line (level=error). In policy mode an
 * error line must be durable when lp_append returns.
 *
 * Each line starts with its sequence number. ackfile (keep it outside the eMMC,
 * e.g. in logs/) always holds three numbers: the last line known durable, the
 * last line generated, and the last error line generated. After a power cut,
 * -c checks the file against them.
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
#include "logpolicy.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void die(const char *what) {
    perror(what);
    exit(1);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: logger -f FILE [-m line|batch|nosync|policy] [-n lines] "
            "[-s line_bytes] [-r lines_per_s]\n"
            "              [-b buf_kb] [-k lines_per_sync] [-t max_delay_ms] "
            "[-e error_every] [-y fsync|fdatasync] [-a ackfile]\n"
            "       logger -c FILE -a ackfile\n");
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
        if (!lat) {
            die("realloc");
        }
    }
    lat[nlat++] = ns;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint64_t pct(double p)
{
    if (!nlat) {
        return 0;
    }
    return lat[(size_t)(p / 100.0 * (double)(nlat - 1) + 0.5)];
}

/* ---------- ack file: durable, generated, last error ---------- */
static int ackfd = -1;
static uint64_t ack_durable, ack_generated, ack_error;

static void ack_write(void)
{
    if (ackfd < 0) {
        return;
    }
    char rec[80];
    int n = snprintf(rec, sizeof rec, "%020lu %020lu %020lu\n",
                     (unsigned long)ack_durable,
                     (unsigned long)ack_generated,
                     (unsigned long)ack_error);
    if (pwrite(ackfd, rec, (size_t)n, 0) != n) {
        die("ackfile");
    }
}

/* ---------- line, batch and nosync modes ---------- */
static int fd, use_fdatasync;
static uint64_t syncs;

static void write_all(const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("write");
        }
        p += w;
        n -= (size_t)w;
    }
}

static void sync_now(uint64_t last_seq)
{
    uint64_t t0 = now_ns();
    if ((use_fdatasync ? fdatasync(fd) : fsync(fd)) < 0) {
        die("fsync");
    }
    add_lat(now_ns() - t0);
    syncs++;
    ack_durable = last_seq;
}

static int make_line(char *buf, size_t size, uint64_t seq, double t, int error)
{
    int n = snprintf(buf, size, "%08lu %011.3f service=epg level=%s msg=",
                     (unsigned long)seq, t, error ? "error" : "info");
    for (; (size_t)n < size - 1; n++) {
        buf[n] = (char)('a' + (seq + (uint64_t)n) % 26);
    }
    buf[size - 1] = '\n';
    return (int)size;
}

/* ---------- checking after a power cut ---------- */
static int check(const char *file, const char *ackfile)
{
    unsigned long durable = 0, generated = 0, error = 0, seq, expect = 1, last = 0;
    int bad = 0;
    FILE *a = fopen(ackfile, "r");
    if (a) {
        if (fscanf(a, "%lu %lu %lu", &durable, &generated, &error) < 1) {
            durable = 0;
        }
        fclose(a);
    }
    FILE *f = fopen(file, "r");
    if (!f)                       /* the file's creation itself never reached NAND */
        printf("%s does not exist after the power cut\n", file);
    char line[8192];
    while (f && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lu", &seq) != 1 || seq != expect) {
            bad = 1;
            break;
        }
        last = seq;
        expect++;
    }
    if (f) {
        fclose(f);
    }
    printf("last line in file        %lu\n", last);
    printf("last line acked durable  %lu\n", durable);
    printf("last line generated      %lu\n", generated);
    printf("last error line          %lu\n", error);
    printf("lines lost               %lu\n", generated > last ? generated - last : 0);
    if (bad) {
        printf("file ends with a line that is not the next sequence number "
               "(torn or zero-filled tail)\n");
    }
    int ok = last >= durable && last >= error;
    if (ok) {
        printf("result: PASS  every acked line and every error line survived\n");
    } else {
        printf("result: FAIL  acked or error lines were lost "
               "(%lu acked, %lu error, file has %lu)\n",
               durable, error, last);
    }
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *file = NULL, *mode = "line", *ackfile = NULL, *checkfile = NULL;
    long lines = 2000, line_bytes = 200, rate = 0, buf_kb = 64, delay_ms = 0, error_every = 0,
        per_sync = 0;
    int c;
    while ((c = getopt(argc, argv, "f:m:n:s:r:b:k:t:e:y:a:c:")) != -1) {
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
            buf_kb = atol(optarg);
            break;
        case 'k':
            per_sync = atol(optarg);
            break;
        case 't':
            delay_ms = atol(optarg);
            break;
        case 'e':
            error_every = atol(optarg);
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
        default: usage();
        }
    }
    if (checkfile) {
        if (!ackfile) {
            usage();
        }
        return check(checkfile, ackfile);
    }
    if (!file || line_bytes < 48 || line_bytes > 4096 || lines < 1) {
        usage();
    }
    int m_line = !strcmp(mode, "line"), m_batch = !strcmp(mode, "batch");
    int m_nosync = !strcmp(mode, "nosync"), m_policy = !strcmp(mode, "policy");
    if (!m_line && !m_batch && !m_nosync && !m_policy) {
        usage();
    }
    if (ackfile && (ackfd = open(ackfile, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
        die(ackfile);

    struct logpolicy *lp = NULL;
    if (m_policy) {
        struct lp_params p = {
            file,
            (uint32_t)buf_kb,
            (uint32_t)delay_ms,
            use_fdatasync,
        };
        if (!(lp = lp_open(&p))) {
            die("lp_open");
        }
    } else if ((fd = open(file, O_WRONLY | O_CREAT | O_APPEND | O_TRUNC, 0644)) < 0) {
        die(file);
    }

    /* -k lines must fit in the buffer */
    if (per_sync > 0 &&
        (size_t)per_sync * (size_t)line_bytes > (size_t)buf_kb * 1024) {
        buf_kb = (per_sync * line_bytes + 1023) / 1024;
    }
    size_t cap = (size_t)buf_kb * 1024 + (size_t)line_bytes;
    char *buf = malloc(cap), *line = malloc((size_t)line_bytes);
    size_t used = 0;
    uint64_t t_start = now_ns(), t_oldest = 0, app_bytes = 0, errors = 0;

    for (long i = 1; i <= lines; i++) {
        if (rate > 0) {                             /* keep the requested line rate */
            uint64_t due = t_start + (uint64_t)(i - 1) * 1000000000ull / (uint64_t)rate;
            if (m_policy)
                /* let the policy flush old lines while it waits */
                while (now_ns() + 1000000ull < due) {
                    if (lp_tick(lp, now_ns()) < 0) {
                        die("lp_tick");
                    }
                    if (lp_durable_seq(lp) != ack_durable) {
                        ack_durable = lp_durable_seq(lp);
                        ack_write();
                    }
                    struct timespec ms = { 0, 1000000 };
                    nanosleep(&ms, NULL);
                }
            struct timespec ts = { (time_t)(due / 1000000000ull), (long)(due % 1000000000ull) };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        }
        uint64_t t = now_ns();
        int is_err = error_every > 0 && i % error_every == 0;
        int n = make_line(line, (size_t)line_bytes, (uint64_t)i, (double)(t - t_start) / 1e9,
            is_err);
        app_bytes += (uint64_t)n;
        errors += (uint64_t)is_err;
        ack_generated = (uint64_t)i;
        if (is_err) {
            ack_error = (uint64_t)i;
        }
        if (m_policy) {
            uint64_t t0 = now_ns(), before = lp_syncs(lp);
            if (lp_append(lp, line, (size_t)n, is_err, (uint64_t)i, t) < 0) {
                die("lp_append");
            }
            if (lp_syncs(lp) != before) {
                add_lat(now_ns() - t0);
            }
            if (is_err && lp_durable_seq(lp) < (uint64_t)i) {
                fprintf(stderr, "logger: error line %ld is not durable after lp_append\n", i);
                exit(3);
            }
            ack_durable = lp_durable_seq(lp);
        } else if (m_line) {
            write_all(line, (size_t)n);
            sync_now((uint64_t)i);
        } else if (m_nosync) {
            write_all(line, (size_t)n);
        } else {
            if (!used) {
                t_oldest = t;
            }
            memcpy(buf + used, line, (size_t)n);
            used += (size_t)n;
            int full = per_sync > 0 ? i % per_sync == 0 : used >= (size_t)buf_kb * 1024;
            int old = delay_ms > 0 && t - t_oldest >= (uint64_t)delay_ms * 1000000ull;
            if (full || old || is_err || i == lines) {
                write_all(buf, used);
                sync_now((uint64_t)i);
                used = 0;
            }
        }
        ack_write();
    }
    if (m_policy) {
        uint64_t s = lp_syncs(lp);
        if (lp_close(lp) < 0) {
            die("lp_close");
        }
        syncs = s + 1;
        ack_durable = (uint64_t)lines;
        ack_write();
    } else {
        if (m_nosync) { /* nothing */ }
        close(fd);
    }
    double el = (double)(now_ns() - t_start) / 1e9;
    qsort(lat, nlat, sizeof *lat, cmp_u64);
    printf("mode=%s lines=%ld line_bytes=%ld errors=%lu app_bytes=%lu "
           "elapsed_s=%.3f syncs=%lu\n",
           mode, lines, line_bytes, (unsigned long)errors,
           (unsigned long)app_bytes, el, (unsigned long)syncs);
    if (nlat)
        printf("sync latency: p50 %.0f us  p99 %.0f us  max %.0f us  (%lu samples, %s)\n",
               pct(50) / 1e3, pct(99) / 1e3, lat[nlat - 1] / 1e3, (unsigned long)nlat,
               use_fdatasync ? "fdatasync" : "fsync");
    return 0;
}
