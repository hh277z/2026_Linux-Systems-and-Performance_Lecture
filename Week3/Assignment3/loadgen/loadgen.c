#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* One outstanding request per persistent TCP connection. No client threads.
 * CLOCK_MONOTONIC measures end-to-end latency from first send attempt to LF.
 * A failed request retires its connection; BUSY consumes one request, no retry.
 */
#define VERSION "1.0.1"
#define LINE_MAX_BYTES 256
#define MAX_SAMPLES 2000000
#define EVENT_BATCH 256

enum workload { W_ECHO, W_TIME, W_COMPUTE, W_MIXED, W_FUNCTIONAL, W_RESPONSIVE };

enum command { C_ECHO, C_TIME, C_COMPUTE, C_QUIT };

enum status { S_OK, S_BUSY, S_PROTOCOL, S_IO, S_TIMEOUT, S_CANCELLED, S_COUNT };

enum state { IDLE, SENDING, READING, DONE };

static const char *work_names[] = {"echo", "time", "compute", "mixed", "functional", "responsive"};
static const char *cmd_names[] = {"echo", "time", "compute", "quit"};
static const char *status_names[] = {"success",          "busy",    "protocol_error",
                                     "connection_error", "timeout", "cancelled"};

typedef struct {
    const char *ip, *json, *label;
    int port, clients, requests, percent;
    unsigned seed;
    unsigned long long work;
    enum workload workload;
    double timeout, connect_timeout, max_seconds, interval, backoff, probe_delay, fragment_delay;
} Config;

typedef struct {
    int client, sequence;
    enum command command;
    enum status status;
    double start, end;
} Sample;

typedef struct {
    int fd, id, seq, target;
    enum state state;
    enum command command;
    char tx[LINE_MAX_BYTES + 1], rx[LINE_MAX_BYTES + 1], expected[LINE_MAX_BYTES + 1];
    size_t tx_len, tx_sent, rx_len;
    double started, due;
} Client;

typedef struct {
    Sample *samples;
    size_t used, planned;
    double origin, elapsed, setup_seconds, cpu_seconds, compute_sent;
    int connected, setup_errors;
    bool deadline, conclusive;
} Run;

typedef struct {
    size_t n;
    double mean, p50, p95, p99, max;
} Stats;

static volatile sig_atomic_t interrupted;

static void on_signal(int signo)
{
    (void)signo;
    interrupted = 1;
}

static double now(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        perror("clock_gettime");
        exit(2);
    }

    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static double cpu_now(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0)
        return 0;

    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int milliseconds(double seconds)
{
    if (seconds <= 0)
        return 0;

    return seconds > 1.0 ? 1000 : (int)ceil(seconds * 1000);
}

static void *allocate(size_t n, size_t size)
{
    void *p = calloc(n, size);

    if (!p) {
        perror("calloc");
        exit(2);
    }

    return p;
}

static void usage(FILE *out)
{
    fprintf(
        out,
        "Usage: ./loadgen <server-ip> <port> <clients> <requests-per-client> <workload> [options]\n"
        "Workloads: echo, time, compute, mixed, functional, responsive\n"
        "  --work N                 compute argument (default 1000000; server defines units)\n"
        "  --compute-percent N      mixed compute share 0..100 (default 20)\n"
        "  --seed N                 deterministic mixed/echo seed (default 1)\n"
        "  --timeout SEC            connect/request deadline (default 5)\n"
        "  --connect-timeout SEC    all-clients connection deadline (default 10)\n"
        "  --max-seconds SEC        whole measured phase/suite deadline (default 120)\n"
        "  --interval-ms MS         per-client pause after response (default 0)\n"
        "  --busy-backoff-ms MS     extra pause after BUSY; no retries (default 0)\n"
        "  --probe-delay-ms MS      responsive light-probe delay after compute send (default 20)\n"
        "  --fragment-delay-ms MS   functional split-write delay (default 40)\n"
        "  --json PATH              save configuration, summary, and request samples\n"
        "  --label TEXT             server configuration label\n"
        "Exit: 0 valid run (BUSY allowed), 1 failed/inconclusive, 2 usage/setup, 130 interrupted\n");
}

static unsigned long long integer(const char *s, unsigned long long lo, unsigned long long hi)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);

    if (!*s || *s == '-' || *end || errno || v < lo || v > hi) {
        fprintf(stderr, "Invalid integer: %s (range %llu..%llu)\n", s, lo, hi);
        exit(2);
    }

    return v;
}

static double number(const char *s, double lo, double hi)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);

    if (!*s || *end || errno || !isfinite(v) || v < lo || v > hi) {
        fprintf(stderr, "Invalid number: %s (range %.3f..%.3f)\n", s, lo, hi);
        exit(2);
    }

    return v;
}

static Config parse(int argc, char **argv)
{
    Config c = {.label = "",
                .percent = 20,
                .seed = 1,
                .work = 1000000,
                .timeout = 5,
                .connect_timeout = 10,
                .max_seconds = 120,
                .probe_delay = .020,
                .fragment_delay = .040};
    static const struct option opts[] = {{"work", 1, NULL, 'w'},
                                         {"compute-percent", 1, NULL, 'p'},
                                         {"seed", 1, NULL, 's'},
                                         {"timeout", 1, NULL, 't'},
                                         {"connect-timeout", 1, NULL, 'c'},
                                         {"max-seconds", 1, NULL, 'm'},
                                         {"interval-ms", 1, NULL, 'i'},
                                         {"busy-backoff-ms", 1, NULL, 'b'},
                                         {"probe-delay-ms", 1, NULL, 'd'},
                                         {"fragment-delay-ms", 1, NULL, 'f'},
                                         {"json", 1, NULL, 'j'},
                                         {"label", 1, NULL, 'l'},
                                         {"help", 0, NULL, 'h'},
                                         {"version", 0, NULL, 'v'},
                                         {NULL, 0, NULL, 0}};
    int o;

    while ((o = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (o) {
        case 'w':
            c.work = integer(optarg, 1, LLONG_MAX);
            break;
        case 'p':
            c.percent = (int)integer(optarg, 0, 100);
            break;
        case 's':
            c.seed = (unsigned)integer(optarg, 0, UINT_MAX);
            break;
        case 't':
            c.timeout = number(optarg, .01, 3600);
            break;
        case 'c':
            c.connect_timeout = number(optarg, .01, 3600);
            break;
        case 'm':
            c.max_seconds = number(optarg, .05, 86400);
            break;
        case 'i':
            c.interval = number(optarg, 0, 60000) / 1000;
            break;
        case 'b':
            c.backoff = number(optarg, 0, 60000) / 1000;
            break;
        case 'd':
            c.probe_delay = number(optarg, 0, 60000) / 1000;
            break;
        case 'f':
            c.fragment_delay = number(optarg, 1, 10000) / 1000;
            break;
        case 'j':
            c.json = optarg;
            break;
        case 'l':
            c.label = optarg;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        case 'v':
            puts(VERSION);
            exit(0);
        default:
            usage(stderr);
            exit(2);
        }
    }

    if (argc - optind != 5) {
        usage(stderr);
        exit(2);
    }

    c.ip = argv[optind];
    struct in_addr address;

    if (inet_pton(AF_INET, c.ip, &address) != 1) {
        fputs("Use a numeric IPv4 address.\n", stderr);
        exit(2);
    }

    c.port = (int)integer(argv[optind + 1], 1, 65535);
    c.clients = (int)integer(argv[optind + 2], 1, 10000);
    c.requests = (int)integer(argv[optind + 3], 1, 10000000);
    int w;

    for (w = 0; w < 6 && strcmp(argv[optind + 4], work_names[w]); ++w) {
    }

    if (w == 6) {
        fputs("Unknown workload.\n", stderr);
        exit(2);
    }

    c.workload = (enum workload)w;

    if (w == W_RESPONSIVE && c.clients < 2) {
        fputs("responsive needs >=2 clients.\n", stderr);
        exit(2);
    }

    if (w != W_FUNCTIONAL && (size_t)c.clients * (size_t)c.requests > MAX_SAMPLES) {
        fputs("At most 2,000,000 samples per run; use repeated runs.\n", stderr);
        exit(2);
    }

    struct rlimit limit;

    if (getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur < (rlim_t)c.clients + 32) {
        fprintf(stderr, "Increase ulimit -n to at least %d (current %llu).\n", c.clients + 32,
                (unsigned long long)limit.rlim_cur);
        exit(2);
    }

    return c;
}

static int new_socket(const Config *c)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    int one = 1;

    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        close(fd);

        return -1;
    }

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)c->port)};
    (void)inet_pton(AF_INET, c->ip, &addr.sin_addr);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
        close(fd);

        return -1;
    }

    return fd;
}

static int ready(int fd, short events, double deadline)
{
    while (!interrupted) {
        double left = deadline - now();

        if (left <= 0) {
            errno = ETIMEDOUT;

            return -1;
        }

        struct pollfd p = {.fd = fd, .events = events};
        int n = poll(&p, 1, milliseconds(left));

        if (n > 0)
            return 0; /* Caller handles EOF, SO_ERROR, partial I/O. */
        if (n < 0 && errno != EINTR)
            return -1;
    }

    errno = EINTR;

    return -1;
}

static int connected_error(int fd)
{
    int error = 0;
    socklen_t len = sizeof(error);

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
        return errno;

    return error;
}

static bool valid_time(const char *s)
{
    if (strlen(s) != 19)
        return false;

    for (int i = 0; i < 19; ++i) {
        char expected = i == 4 || i == 7 ? '-' : i == 10 ? ' ' : i == 13 || i == 16 ? ':' : 0;

        if (expected ? s[i] != expected : s[i] < '0' || s[i] > '9')
            return false;
    }

    int y, m, d, h, mi, se;

    if (sscanf(s, "%d-%d-%d %d:%d:%d", &y, &m, &d, &h, &mi, &se) != 6)
        return false;
    static const int days[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (y < 1 || m < 1 || m > 12 || d < 1 || h > 23 || mi > 59 || se > 59)
        return false;
    int leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);

    return d <= days[m] + (m == 2 && leap);
}

static enum status validate(char *line, size_t len, enum command command, const char *expected)
{
    if (!len || len > LINE_MAX_BYTES || line[len - 1] != '\n')
        return S_PROTOCOL;
    --len;

    if (len && line[len - 1] == '\r')
        --len;

    for (size_t i = 0; i < len; ++i)
        if ((unsigned char)line[i] < 32 || (unsigned char)line[i] > 126)
            return S_PROTOCOL;
    line[len] = '\0';

    if (!strcmp(line, "BUSY"))
        return command == C_COMPUTE ? S_BUSY : S_PROTOCOL;

    if (command == C_ECHO)
        return !strcmp(line, expected) ? S_OK : S_PROTOCOL;

    if (command == C_TIME)
        return !strncmp(line, "TIME ", 5) && valid_time(line + 5) ? S_OK : S_PROTOCOL;

    if (command == C_QUIT)
        return !strcmp(line, "BYE") ? S_OK : S_PROTOCOL;

    if (strncmp(line, "RESULT ", 7))
        return S_PROTOCOL;

    for (size_t i = 7; i < len; ++i)
        if (line[i] != ' ')
            return S_OK;

    return S_PROTOCOL;
}

static void prepare(Client *p, const Config *c)
{
    enum command command = C_ECHO;

    switch (c->workload) {
    case W_ECHO:
        command = C_ECHO;
        break;
    case W_TIME:
        command = C_TIME;
        break;
    case W_COMPUTE:
        command = C_COMPUTE;
        break;
    case W_MIXED: {
        unsigned slot =
            (unsigned)(((uint64_t)p->seq * 37u + (uint64_t)p->id * 17u + c->seed) % 100u);
        command = slot < (unsigned)c->percent ? C_COMPUTE : slot % 2 ? C_ECHO : C_TIME;
        break;
    }
    case W_RESPONSIVE:
        command = p->id == 0 ? C_COMPUTE : p->seq % 2 ? C_TIME : C_ECHO;
        break;
    default:
        break;
    }

    p->command = command;

    if (command == C_ECHO) {
        snprintf(p->tx, sizeof(p->tx), "echo lg-%u-%d-%d\n", c->seed, p->id, p->seq);
        snprintf(p->expected, sizeof(p->expected), "OK lg-%u-%d-%d", c->seed, p->id, p->seq);
    } else if (command == C_TIME)
        strcpy(p->tx, "time\n");
    else
        snprintf(p->tx, sizeof(p->tx), "compute %llu\n", c->work);
    p->tx_len = strlen(p->tx);
    p->tx_sent = 0;
    p->rx_len = 0;
    p->started = now();
    p->state = SENDING;
}

static bool watch(int ep, Client *p, uint32_t events, int operation)
{
    struct epoll_event e = {.events = events, .data.u32 = (uint32_t)p->id};

    return epoll_ctl(ep, operation, p->fd, &e) == 0;
}

static void retire(Client *p, int ep)
{
    if (p->fd >= 0) {
        (void)epoll_ctl(ep, EPOLL_CTL_DEL, p->fd, NULL);
        close(p->fd);
        p->fd = -1;
    }

    p->state = DONE;
}

static void finish(Client *p, int ep, Run *r, const Config *c, enum status status)
{
    Sample *s = &r->samples[r->used++];
    *s = (Sample){.client = p->id,
                  .sequence = p->seq,
                  .command = p->command,
                  .status = status,
                  .start = p->started - r->origin,
                  .end = now() - r->origin};
    ++p->seq;

    if (status != S_OK && status != S_BUSY) {
        if (r->used <= 10)
            fprintf(stderr, "client=%d request=%d: %s\n", p->id, p->seq - 1, status_names[status]);
        retire(p, ep);
        return;
    }

    if (p->seq >= p->target) {
        retire(p, ep);
        return;
    }

    p->state = IDLE;
    p->due = now() + c->interval + (status == S_BUSY ? c->backoff : 0);

    if (!watch(ep, p, EPOLLIN, EPOLL_CTL_MOD))
        retire(p, ep);
}

static void send_pending(Client *p, int ep, Run *r, const Config *c)
{
    while (p->tx_sent < p->tx_len) {
        ssize_t n = send(p->fd, p->tx + p->tx_sent, p->tx_len - p->tx_sent, MSG_NOSIGNAL);

        if (n > 0)
            p->tx_sent += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!watch(ep, p, EPOLLOUT, EPOLL_CTL_MOD))
                finish(p, ep, r, c, S_IO);
            return;
        } else {
            finish(p, ep, r, c, S_IO);
            return;
        }
    }

    p->state = READING;

    if (c->workload == W_RESPONSIVE && p->id == 0)
        r->compute_sent = now();

    if (!watch(ep, p, EPOLLIN, EPOLL_CTL_MOD))
        finish(p, ep, r, c, S_IO);
}

static void receive_pending(Client *p, int ep, Run *r, const Config *c)
{
    for (;;) {
        ssize_t n = recv(p->fd, p->rx + p->rx_len, LINE_MAX_BYTES + 1 - p->rx_len, 0);

        if (n > 0) {
            p->rx_len += (size_t)n;
            char *lf = memchr(p->rx, '\n', p->rx_len);

            if (lf) {
                size_t len = (size_t)(lf - p->rx) + 1;
                enum status status =
                    len != p->rx_len ? S_PROTOCOL : validate(p->rx, len, p->command, p->expected);
                finish(p, ep, r, c, status);
                return;
            }

            if (p->rx_len >= LINE_MAX_BYTES) {
                finish(p, ep, r, c, S_PROTOCOL);
                return;
            }
        } else if (n == 0) {
            finish(p, ep, r, c, p->rx_len ? S_PROTOCOL : S_IO);
            return;
        } else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        else {
            finish(p, ep, r, c, S_IO);
            return;
        }
    }
}

static void setup(Client *clients, Run *r, const Config *c)
{
    struct pollfd *fds = allocate((size_t)c->clients, sizeof(*fds));
    double start = now(), deadline = start + fmin(c->timeout, c->connect_timeout);
    int pending = 0;

    for (int i = 0; i < c->clients; ++i) {
        Client *p = &clients[i];
        p->id = i;
        p->target = c->workload == W_RESPONSIVE && i == 0 ? 1 : c->requests;
        p->fd = new_socket(c);
        fds[i] = (struct pollfd){.fd = p->fd, .events = POLLOUT};

        if (p->fd < 0)
            ++r->setup_errors;
        else
            ++pending;
    }

    while (pending && !interrupted && now() < deadline) {
        int n = poll(fds, (nfds_t)c->clients, milliseconds(deadline - now()));

        if (n < 0 && errno == EINTR)
            continue;

        if (n < 0)
            break;

        for (int i = 0; i < c->clients; ++i)
            if (fds[i].fd >= 0 && fds[i].revents) {
                int err = connected_error(fds[i].fd);

                if (err) {
                    ++r->setup_errors;
                    close(clients[i].fd);
                    clients[i].fd = -1;
                } else
                    ++r->connected;
                fds[i].fd = -1;
                --pending;
            }
    }

    r->setup_errors += pending;

    for (int i = 0; i < c->clients; ++i)
        if (fds[i].fd >= 0) {
            close(clients[i].fd);
            clients[i].fd = -1;
        }
    r->setup_seconds = now() - start;
    free(fds);
}

static Run benchmark(const Config *c)
{
    Run r = {.compute_sent = -1};
    r.planned = c->workload == W_RESPONSIVE ? 1 + (size_t)(c->clients - 1) * c->requests
                                            : (size_t)c->clients * c->requests;
    r.samples = allocate(r.planned, sizeof(*r.samples));
    Client *clients = allocate((size_t)c->clients, sizeof(*clients));
    setup(clients, &r, c);
    int ep = epoll_create1(EPOLL_CLOEXEC);

    if (ep < 0) {
        perror("epoll_create1");
        exit(2);
    }

    r.origin = now();
    double cpu_start = cpu_now();

    if (r.setup_errors || interrupted)
        goto done;

    for (int i = 0; i < c->clients; ++i) {
        clients[i].due = r.origin;

        if (!watch(ep, &clients[i], EPOLLIN, EPOLL_CTL_ADD)) {
            ++r.setup_errors;
            goto done;
        }
    }

    for (;;) {
        double t = now(), wake = r.origin + c->max_seconds;
        int alive = 0;

        if (interrupted || t >= r.origin + c->max_seconds) {
            r.deadline = !interrupted;

            for (int i = 0; i < c->clients; ++i) {
                Client *p = &clients[i];

                if (p->state == SENDING || p->state == READING)
                    finish(p, ep, &r, c, S_CANCELLED);
            }

            break;
        }

        for (int i = 0; i < c->clients; ++i) {
            Client *p = &clients[i];

            if (p->state == DONE)
                continue;
            ++alive;

            if (p->state == IDLE) {
                if (c->workload == W_RESPONSIVE && i > 0 && p->seq == 0) {
                    if (r.compute_sent < 0) {
                        p->due = r.origin + c->max_seconds;
                    } else
                        p->due = r.compute_sent + c->probe_delay;
                }

                if (p->due <= t) {
                    prepare(p, c);
                    send_pending(p, ep, &r, c);
                } else
                    wake = fmin(wake, p->due);
            }

            if (p->state == READING || p->state == SENDING) {
                if (t >= p->started + c->timeout)
                    finish(p, ep, &r, c, S_TIMEOUT);
                else
                    wake = fmin(wake, p->started + c->timeout);
            }
        }

        if (!alive)
            break;
        /* A timeout/send failure above may have retired the last socket. */
        alive = 0;

        for (int i = 0; i < c->clients; ++i)
            if (clients[i].state != DONE)
                ++alive;

        if (!alive ||
            (c->workload == W_RESPONSIVE && clients[0].state == DONE && r.compute_sent < 0))
            break;
        struct epoll_event events[EVENT_BATCH];
        int n = epoll_wait(ep, events, EVENT_BATCH, milliseconds(wake - now()));

        if (n < 0 && errno == EINTR)
            continue;

        if (n < 0) {
            perror("epoll_wait");
            ++r.setup_errors;
            break;
        }

        for (int i = 0; i < n; ++i) {
            Client *p = &clients[events[i].data.u32];

            if (p->state == SENDING)
                send_pending(p, ep, &r, c);
            else if (p->state == READING)
                receive_pending(p, ep, &r, c);
            else if (p->state == IDLE) {
                /* Unsolicited data or EOF between requests invalidates this connection. */
                char b;
                ssize_t got = recv(p->fd, &b, 1, MSG_PEEK);

                if (got >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    prepare(p, c);
                    finish(p, ep, &r, c, got > 0 ? S_PROTOCOL : S_IO);
                }
            }
        }
    }
done:
    r.elapsed = now() - r.origin;
    r.cpu_seconds = cpu_now() - cpu_start;

    for (int i = 0; i < c->clients; ++i)
        if (clients[i].fd >= 0)
            close(clients[i].fd);
    close(ep);
    free(clients);

    return r;
}

/* Functional tests deliberately use bounded poll/read/write helpers. They do not
 * produce benchmark rates. Per-test sockets are always closed by the harness. */
typedef struct {
    int fd;
    char data[8192];
    size_t used;
} Peer;

typedef struct {
    const char *name;
    bool passed;
    char detail[160];
} Check;

static char check_detail[160];
static double functional_deadline;

static void detail(const char *s)
{
    snprintf(check_detail, sizeof(check_detail), "%s", s);
}

static double request_deadline(const Config *c)
{
    return fmin(now() + c->timeout, functional_deadline);
}

static bool peer_open(Peer *p, const Config *c)
{
    p->fd = new_socket(c);
    p->used = 0;

    if (p->fd < 0 || ready(p->fd, POLLOUT, request_deadline(c)) < 0) {
        detail("connection failed/timed out");

        return false;
    }

    int err = connected_error(p->fd);

    if (err) {
        detail(strerror(err));

        return false;
    }

    return true;
}

static bool write_all(Peer *p, const void *data, size_t size, double deadline)
{
    const char *s = data;

    while (size && !interrupted) {
        if (now() >= deadline) {
            detail("write deadline exceeded");

            return false;
        }

        ssize_t n = send(p->fd, s, size, MSG_NOSIGNAL);

        if (n > 0) {
            s += n;
            size -= (size_t)n;
        } else if (n < 0 && errno == EINTR)
            continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (ready(p->fd, POLLOUT, deadline) < 0) {
                detail("write timeout");

                return false;
            }
        } else {
            detail("write failed");

            return false;
        }
    }

    return !interrupted;
}

static bool read_valid(Peer *p, enum command cmd, const char *expected, double deadline)
{
    for (;;) {
        char *lf = memchr(p->data, '\n', p->used);

        if (lf) {
            size_t len = (size_t)(lf - p->data) + 1;

            if (validate(p->data, len, cmd, expected) != S_OK) {
                detail("invalid or unexpected response");

                return false;
            }

            memmove(p->data, p->data + len, p->used - len);
            p->used -= len;

            return true;
        }

        if (p->used >= LINE_MAX_BYTES) {
            detail("response line too long");

            return false;
        }

        if (ready(p->fd, POLLIN, deadline) < 0) {
            detail("response timeout");

            return false;
        }

        ssize_t n = recv(p->fd, p->data + p->used, sizeof(p->data) - p->used, 0);

        if (n > 0)
            p->used += (size_t)n;
        else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        else {
            detail("EOF/socket error before complete response");

            return false;
        }
    }
}

static bool exchange(Peer *p, const char *data, enum command cmd, const char *expected,
                     const Config *c)
{
    double deadline = request_deadline(c);

    return write_all(p, data, strlen(data), deadline) && read_valid(p, cmd, expected, deadline);
}

static bool healthy(const Config *c)
{
    Peer p = {.fd = -1};
    bool ok = peer_open(&p, c) && exchange(&p, "echo still-alive\n", C_ECHO, "OK still-alive", c);

    if (p.fd >= 0)
        close(p.fd);

    return ok;
}

static bool client_buffer_isolation(const Config *c)
{
    Peer a = {.fd = -1}, b = {.fd = -1};
    bool ok = false;

    if (!peer_open(&a, c) || !peer_open(&b, c))
        goto done;

    if (!write_all(&a, "echo client-a-", 14, request_deadline(c)))
        goto done;

    /* Keep A incomplete while B completes a different request. This also catches
     * a server that blocks all clients while waiting for A's terminating LF. */
    double until = fmin(now() + c->fragment_delay, functional_deadline);

    if (ready(a.fd, POLLIN, until) == 0) {
        detail("client A responded or closed before its request LF");
        goto done;
    }

    if (interrupted || now() >= functional_deadline)
        goto done;

    ok = exchange(&b, "echo client-b\n", C_ECHO, "OK client-b", c) &&
         exchange(&a, "finished\n", C_ECHO, "OK client-a-finished", c);

done:
    if (a.fd >= 0)
        close(a.fd);

    if (b.fd >= 0)
        close(b.fd);

    return ok;
}

static bool functional_case(int test, const Config *c)
{
    Peer p = {.fd = -1};
    bool ok = false;

    if (test == 0) {
        int n = c->clients < 3 ? 3 : c->clients;
        Peer *peers = allocate((size_t)n, sizeof(*peers));

        for (int i = 0; i < n; ++i)
            peers[i].fd = -1;
        ok = true;

        for (int i = 0; i < n && ok; ++i)
            ok = peer_open(&peers[i], c);
        /* Send to every live client before reading any responses. */
        for (int i = 0; i < n && ok; ++i) {
            char request[64];
            snprintf(request, sizeof(request), "echo concurrent-%d\n", i);
            ok = write_all(&peers[i], request, strlen(request), request_deadline(c));
        }

        for (int i = 0; i < n && ok; ++i) {
            char expected[64];
            snprintf(expected, sizeof(expected), "OK concurrent-%d", i);
            ok = read_valid(&peers[i], C_ECHO, expected, request_deadline(c));
        }

        for (int i = 0; i < n; ++i)
            if (peers[i].fd >= 0)
                close(peers[i].fd);
        free(peers);

        return ok;
    }

    if (test == 10)
        return client_buffer_isolation(c);

    if (!peer_open(&p, c))
        goto end;

    if (test == 1) {
        if (!write_all(&p, "echo frag", 9, request_deadline(c)))
            goto end;
        double until = fmin(now() + c->fragment_delay, functional_deadline);

        if (ready(p.fd, POLLIN, until) == 0) {
            detail("response or EOF before request LF");
            goto end;
        }

        if (interrupted || now() >= functional_deadline)
            goto end;
        ok = exchange(&p, "mented\n", C_ECHO, "OK fragmented", c);
    } else if (test == 2) {
        const char *data = "echo first\ntime\necho third\n";
        ok = write_all(&p, data, strlen(data), request_deadline(c)) &&
             read_valid(&p, C_ECHO, "OK first", request_deadline(c)) &&
             read_valid(&p, C_TIME, "", request_deadline(c)) &&
             read_valid(&p, C_ECHO, "OK third", request_deadline(c));
    } else if (test == 3) {
        const char *data = "echo first\necho sec";
        ok = write_all(&p, data, strlen(data), request_deadline(c)) &&
             read_valid(&p, C_ECHO, "OK first", request_deadline(c)) &&
             exchange(&p, "ond\n", C_ECHO, "OK second", c);
    } else if (test == 4) {
        char data[257], expected[254];
        memcpy(data, "echo ", 5);
        memset(data + 5, 'x', 250);
        data[255] = '\n';
        data[256] = 0;
        memcpy(expected, "OK ", 3);
        memset(expected + 3, 'x', 250);
        expected[253] = 0;
        ok = exchange(&p, data, C_ECHO, expected, c);
    } else if (test == 5) {
        if (!exchange(&p, "quit\n", C_QUIT, "BYE", c))
            goto end;
        char b;

        if (p.used || ready(p.fd, POLLIN, request_deadline(c)) < 0 || recv(p.fd, &b, 1, 0) != 0) {
            detail("expected connection close after BYE");
            goto end;
        }

        ok = healthy(c);
    } else if (test == 6 || test == 7) {
        if (!write_all(&p, "echo unfinished", 15, request_deadline(c)))
            goto end;

        if (test == 7) {
            struct linger linger = {.l_onoff = 1, .l_linger = 0};

            if (setsockopt(p.fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger)) < 0) {
                detail("cannot force RST");
                goto end;
            }
        }

        close(p.fd);
        p.fd = -1;
        ok = healthy(c);
    } else {
        char data[4103];

        if (test == 8)
            strcpy(data, "not_a_command\ncompute nope\n");
        else {
            memcpy(data, "echo ", 5);
            memset(data + 5, 'x', 4096);
            data[4101] = '\n';
            data[4102] = 0;
        }

        if (!write_all(&p, data, strlen(data), request_deadline(c)))
            goto end;
        /* No error-response grammar is specified. Only fresh-client survival is scored. */
        double until = fmin(now() + .1, functional_deadline);

        if (ready(p.fd, POLLIN, until) == 0) {
            char discard[512];
            (void)recv(p.fd, discard, sizeof(discard), 0);
        }

        close(p.fd);
        p.fd = -1;
        ok = healthy(c);
    }
end:
    if (p.fd >= 0)
        close(p.fd);

    return ok;
}

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return (x > y) - (x < y);
}

static Stats stats(const Run *r, enum status status, int command, bool overlap,
                   const Sample *compute)
{
    double *values = allocate(r->used ? r->used : 1, sizeof(*values));
    Stats s = {0};

    for (size_t i = 0; i < r->used; ++i) {
        const Sample *p = &r->samples[i];

        if (p->status != status || (command >= 0 && p->command != (enum command)command))
            continue;

        if (overlap && (!compute || p->command == C_COMPUTE || p->start < compute->start ||
                        p->start >= compute->end))
            continue;
        double ms = (p->end - p->start) * 1000;
        values[s.n++] = ms;
        s.mean += ms;
    }

    if (s.n) {
        qsort(values, s.n, sizeof(*values), compare_double);
        s.mean /= s.n;
        s.p50 = values[(size_t)ceil(s.n * .50) - 1];
        s.p95 = values[(size_t)ceil(s.n * .95) - 1];
        s.p99 = values[(size_t)ceil(s.n * .99) - 1];
        s.max = values[s.n - 1];
    }

    free(values);

    return s;
}

static void json_string(FILE *f, const char *s)
{
    fputc('"', f);

    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', f);
            fputc(*p, f);
        } else if (*p < 32)
            fprintf(f, "\\u%04x", *p);
        else
            fputc(*p, f);
    }

    fputc('"', f);
}

static void json_stats(FILE *f, Stats s)
{
    fprintf(f, "{\"count\":%zu,", s.n);

    if (s.n)
        fprintf(f, "\"mean\":%.9f,\"p50\":%.9f,\"p95\":%.9f,\"p99\":%.9f,\"max\":%.9f}", s.mean,
                s.p50, s.p95, s.p99, s.max);
    else
        fputs("\"mean\":null,\"p50\":null,\"p95\":null,\"p99\":null,\"max\":null}", f);
}

static void json_config(FILE *f, const Config *c)
{
    fputs("{\"schema_version\":1,\"loadgen_version\":\"" VERSION "\",\"config\":{\"server_ip\":",
          f);
    json_string(f, c->ip);
    fprintf(f, ",\"port\":%d,\"clients\":%d,\"requests_per_client\":%d,\"workload\":", c->port,
            c->clients, c->requests);
    json_string(f, work_names[c->workload]);
    /* work is serialized as a string to avoid loss in JavaScript consumers. */
    fprintf(
        f,
        ",\"work\":\"%llu\",\"compute_percent\":%d,\"seed\":%u,\"timeout\":%.9f,"
        "\"connect_timeout\":%.9f,\"max_seconds\":%.9f,\"interval_ms\":%.9f,"
        "\"busy_backoff_ms\":%.9f,\"probe_delay_ms\":%.9f,\"fragment_delay_ms\":%.9f,\"label\":",
        c->work, c->percent, c->seed, c->timeout, c->connect_timeout, c->max_seconds,
        c->interval * 1000, c->backoff * 1000, c->probe_delay * 1000, c->fragment_delay * 1000);
    json_string(f, c->label);
    fputs("}", f);
}

static void print_stats(const char *name, Stats s)
{
    if (!s.n)
        printf("  %s latency: no samples\n", name);
    else
        printf("  %s latency ms: n=%zu mean=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f\n", name, s.n,
               s.mean, s.p50, s.p95, s.p99, s.max);
}

static bool report(Run *r, const Config *c, FILE *f)
{
    size_t counts[S_COUNT] = {0};
    const Sample *compute = NULL;

    for (size_t i = 0; i < r->used; ++i) {
        ++counts[r->samples[i].status];

        if (r->samples[i].command == C_COMPUTE && c->workload == W_RESPONSIVE)
            compute = &r->samples[i];
    }

    size_t responses = counts[S_OK] + counts[S_BUSY], errors = r->used - responses;
    Stats success = stats(r, S_OK, -1, false, NULL), busy = stats(r, S_BUSY, -1, false, NULL);
    Stats overlap = stats(r, S_OK, -1, true, compute);
    r->conclusive = compute && compute->status == S_OK && overlap.n > 0;
    bool ok = !r->setup_errors && !r->deadline && !interrupted && !errors && r->used == r->planned;

    if (c->workload == W_RESPONSIVE && !r->conclusive)
        ok = false;
    double elapsed = r->elapsed > 0 ? r->elapsed : 1e-9;
    double busy_pct = responses ? 100.0 * counts[S_BUSY] / responses : 0;
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL / INCONCLUSIVE");
    printf("  planned=%zu attempted=%zu success=%zu busy=%zu errors=%zu not_sent=%zu\n", r->planned,
           r->used, counts[S_OK], counts[S_BUSY], errors, r->planned - r->used);
    printf("  elapsed=%.6fs success_rps=%.3f response_rps=%.3f busy_percent=%.3f\n", elapsed,
           counts[S_OK] / elapsed, responses / elapsed, busy_pct);
    printf("  connected=%d/%d setup_errors=%d generator_cpu_one_core=%.1f%%\n", r->connected,
           c->clients, r->setup_errors, 100 * r->cpu_seconds / elapsed);
    print_stats("success", success);
    print_stats("BUSY", busy);

    for (int k = 0; k < 3; ++k)
        print_stats(cmd_names[k], stats(r, S_OK, k, false, NULL));
    size_t before = 0;

    if (compute)
        for (size_t i = 0; i < r->used; ++i) {
            Sample *s = &r->samples[i];

            if (s->command != C_COMPUTE && s->status == S_OK && s->start >= compute->start &&
                s->end < compute->end)
                ++before;
        }

    if (c->workload == W_RESPONSIVE) {
        printf("  responsive conclusive=%s light_completed_before_compute_response=%zu\n",
               r->conclusive ? "true" : "false", before);
        print_stats("light sent while compute outstanding", overlap);
        puts(
            "  NOTE: outstanding means sent but not answered; verify actual compute execution in server evidence.");

        if (!r->conclusive)
            puts(
                "  NOTE: increase --work/reduce --probe-delay-ms; compute must succeed and overlap light probes.");
    }

    if (!counts[S_OK])
        puts("  NOTE: no successful requests; no useful server throughput demonstrated.");

    if (!f)
        return ok;
    json_config(f, c);
    fprintf(
        f,
        ",\"ok\":%s,\"interrupted\":%s,\"deadline_exceeded\":%s,\"connected_clients\":%d,\"setup_errors\":%d,"
        "\"connection_setup_seconds\":%.9f,\"generator_cpu_seconds\":%.9f,\"generator_cpu_percent_one_core\":%.9f,"
        "\"summary\":{\"planned\":%zu,\"attempted\":%zu,\"not_sent\":%zu,\"success\":%zu,\"busy\":%zu,"
        "\"errors\":%zu,\"elapsed_seconds\":%.9f,\"success_rps\":%.9f,\"response_rps\":%.9f,\"busy_percent_of_responses\":",
        ok ? "true" : "false", interrupted ? "true" : "false", r->deadline ? "true" : "false",
        r->connected, r->setup_errors, r->setup_seconds, r->cpu_seconds,
        100 * r->cpu_seconds / elapsed, r->planned, r->used, r->planned - r->used, counts[S_OK],
        counts[S_BUSY], errors, elapsed, counts[S_OK] / elapsed, responses / elapsed);

    if (responses)
        fprintf(f, "%.9f", busy_pct);
    else
        fputs("null", f);
    fputs(",\"error_counts\":{", f);

    for (int k = S_PROTOCOL; k < S_COUNT; ++k)
        fprintf(f, "%s\"%s\":%zu", k == S_PROTOCOL ? "" : ",", status_names[k], counts[k]);
    fputs("},\"latency_ms\":{\"success\":", f);
    json_stats(f, success);
    fputs(",\"busy\":", f);
    json_stats(f, busy);
    fputs("},\"by_command\":{", f);

    for (int k = 0; k < 3; ++k) {
        size_t good = 0, rejected = 0;

        for (size_t i = 0; i < r->used; ++i)
            if ((int)r->samples[i].command == k) {
                good += r->samples[i].status == S_OK;
                rejected += r->samples[i].status == S_BUSY;
            }
        fprintf(f, "%s\"%s\":{\"success\":%zu,\"busy\":%zu,\"success_latency_ms\":", k ? "," : "",
                cmd_names[k], good, rejected);
        json_stats(f, stats(r, S_OK, k, false, NULL));
        fputc('}', f);
    }

    fputs("}}", f);

    if (c->workload == W_RESPONSIVE) {
        fprintf(
            f,
            ",\"responsive\":{\"conclusive\":%s,\"light_completed_before_compute_response\":%zu,"
            "\"light_overlap_success_latency_ms\":",
            r->conclusive ? "true" : "false", before);
        json_stats(f, overlap);
        fputc('}', f);
    }

    fputs(",\"samples\":[", f);

    for (size_t i = 0; i < r->used; ++i) {
        const Sample *s = &r->samples[i];
        fprintf(f,
                "%s{\"client\":%d,\"sequence\":%d,\"command\":\"%s\",\"status\":\"%s\","
                "\"start_ms\":%.9f,\"end_ms\":%.9f,\"latency_ms\":%.9f}",
                i ? ",\n" : "", s->client, s->sequence, cmd_names[s->command],
                status_names[s->status], s->start * 1000, s->end * 1000,
                (s->end - s->start) * 1000);
    }

    fputs("]}\n", f);

    return ok;
}

static bool run_functional(const Config *c, FILE *f)
{
    const char *names[] = {"multiple_clients",
                           "partial_command",
                           "multiple_commands_one_write",
                           "complete_line_plus_partial_tail",
                           "256_byte_request",
                           "quit_and_eof",
                           "normal_disconnect",
                           "reset_disconnect",
                           "malformed_service_survival",
                           "oversized_service_survival",
                           "per_client_buffer_isolation"};
    enum { CHECK_COUNT = sizeof(names) / sizeof(names[0]) };
    Check checks[CHECK_COUNT];
    bool all = true;
    functional_deadline = now() + c->max_seconds;

    for (int i = 0; i < CHECK_COUNT; ++i) {
        detail("suite deadline or interruption");
        bool ok = !interrupted && now() < functional_deadline && functional_case(i, c);
        checks[i].name = names[i];
        checks[i].passed = ok;
        snprintf(checks[i].detail, sizeof(checks[i].detail), "%s", ok ? "" : check_detail);
        printf("  %s %s%s%s\n", ok ? "PASS" : "FAIL", names[i], ok ? "" : ": ",
               ok ? "" : check_detail);
        all = all && ok;
    }

    puts(
        "NOTE: malformed/oversized checks assess service survival; error-response grammar is unspecified.");
    puts(
        "NOTE: separate TCP writes do not guarantee separate server reads; inspect server buffering too.");
    printf("RESULT: %s\n", all ? "PASS" : "FAIL");

    if (f) {
        json_config(f, c);
        fprintf(f, ",\"ok\":%s,\"checks\":[", all ? "true" : "false");

        for (int i = 0; i < CHECK_COUNT; ++i) {
            fprintf(f, "%s{\"name\":\"%s\",\"passed\":%s,\"detail\":", i ? "," : "", checks[i].name,
                    checks[i].passed ? "true" : "false");
            json_string(f, checks[i].detail);
            fputc('}', f);
        }

        fputs("]}\n", f);
    }

    return all;
}

int main(int argc, char **argv)
{
    Config c = parse(argc, argv);
    struct sigaction action = {0};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)signal(SIGPIPE, SIG_IGN);
    FILE *json = NULL;

    if (c.json && !(json = fopen(c.json, "w"))) {
        perror(c.json);

        return 2;
    }

    printf("loadgen %s: %s %s:%d clients=%d requests/client=%d\n", VERSION, work_names[c.workload],
           c.ip, c.port, c.clients, c.requests);
    fflush(stdout);
    bool ok;

    if (c.workload == W_FUNCTIONAL)
        ok = run_functional(&c, json);
    else {
        Run r = benchmark(&c);
        ok = report(&r, &c, json);
        free(r.samples);
    }

    if (json) {
        bool bad = ferror(json) != 0;

        if (fclose(json) != 0)
            bad = true;

        if (bad) {
            fputs("Failed to write JSON output.\n", stderr);

            return 2;
        }

        printf("Saved: %s\n", c.json);
    }

    return interrupted ? 130 : ok ? 0 : 1;
}
