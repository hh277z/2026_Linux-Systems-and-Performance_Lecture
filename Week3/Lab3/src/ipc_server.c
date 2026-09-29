#include "lab.h"
#include "log_stats.h"
#include <sys/socket.h>
#include <sys/un.h>

#define CLIENTS 16
#define CAPACITY 512
struct client {
    int fd;
    char input[CAPACITY], output[CAPACITY];
    size_t used, sent, queued;
    bool stats_after_flush;
};

static struct client peers[CLIENTS];
static volatile sig_atomic_t stopping;
static bool rescan_mode;
static struct log_data log_data;
static int epfd;

static void on_stop(int sig) {
    /* Mark the event loop for shutdown when a termination signal arrives. */
    (void)sig;
    stopping = 1;
}

static void disconnect_client(struct client *c) {
    /* Remove the client FD from epoll and reset its connection state. */
    (void)epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static bool queue_reply(struct client *c, const char *reply) {
    /* Append a reply to the client's output buffer. */
    size_t n = strlen(reply);
    if (n > sizeof c->output - c->queued) {
        disconnect_client(c);
        return false;
    }

    memcpy(c->output + c->queued, reply, n);
    c->queued += n;
    return true;
}

static bool flush_reply(struct client *c) {
    /* Send queued replies, then compute stats on this thread after START. */
    for (;;) {
        while (c->sent < c->queued) {
            ssize_t n = send(c->fd, c->output + c->sent, c->queued - c->sent, MSG_NOSIGNAL);
            if (n > 0) {
                c->sent += (size_t)n;
                continue;
            }

            if (n < 0 && errno == EINTR) {
                continue;
            }

            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return true;
            }

            disconnect_client(c);
            return false;
        }

        c->sent = c->queued = 0;
        if (!c->stats_after_flush) {
            return true;
        }

        c->stats_after_flush = false;
        /* START has been sent; the event loop computes here. */
        printf("AGGREGATE_BEGIN fd=%d mode=%s rows=%zu\n", c->fd,
               rescan_mode ? "rescan" : "onepass", log_data.count);
        double start = now_ms();
        struct service_stats stats[SERVICE_COUNT]; /* KEY_STATS_HANDLER */
        if (rescan_mode) {
            aggregate_rescan(&log_data, stats); /* KEY_RESCAN_CALL */
        } else {
            aggregate_onepass(&log_data, stats); /* KEY_ONEPASS_CALL */
        }

        struct totals totals = summarize(stats);
        double elapsed = now_ms() - start;
        char reply[160];
        snprintf(reply, sizeof reply,
                 "STATS requests=%llu errors=%llu duration_us=%llu signature=%llu\n",
                 (unsigned long long)totals.requests, (unsigned long long)totals.errors,
                 (unsigned long long)totals.duration_us, (unsigned long long)totals.signature);
        if (!queue_reply(c, reply)) {
            return false;
        }

        printf("AGGREGATE_END fd=%d compute_ms=%.1f\n", c->fd, elapsed);
    }
}

static bool read_requests(struct client *c) {
    /* Read requests, distinguish STATS, PING, and QUIT, and start replies. */
    /* Newline framing; the teaching client sends one outstanding request per connection. */
    for (;;) {
        if (c->used == sizeof c->input) {
            disconnect_client(c);
            return false;
        }

        ssize_t n = recv(c->fd, c->input + c->used, sizeof c->input - c->used, 0);
        if (!n) {
            disconnect_client(c);
            return false;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }

        if (n < 0) {
            disconnect_client(c);
            return false;
        }

        c->used += (size_t)n;
        char *newline;
        while ((newline = memchr(c->input, '\n', c->used)) != NULL) {
            size_t length = (size_t)(newline - c->input);
            bool is_ping = length == 4 && !memcmp(c->input, "PING", 4);
            bool is_stats = length == 5 && !memcmp(c->input, "STATS", 5);
            bool is_quit = length == 4 && !memcmp(c->input, "QUIT", 4);
            memmove(c->input, newline + 1, c->used - length - 1);
            c->used -= length + 1;
            if (is_quit) {
                if (!queue_reply(c, "BYE\n") || !flush_reply(c)) {
                    return false;
                }

                stopping = 1;
                return true;
            }

            if (is_stats) {
                if (!queue_reply(c, "START\n")) {
                    return false;
                }

                c->stats_after_flush = true;
            } else {
                if (!queue_reply(c, is_ping ? "PONG\n" : "ERROR\n")) {
                    return false;
                }
            }

            /* A long aggregation postpones the next client event. */
            if (!flush_reply(c)) { /* KEY_CALL_HANDLER */
                return false;
            }

            if (c->stats_after_flush) {
                return true;
            }
        }
    }
}

static void watch_client(struct client *c, int slot) {
    /* Update the client's epoll interests based on pending output. */
    struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.u32 = (uint32_t)(slot + 1)};
    if (c->sent < c->queued) {
        ev.events |= EPOLLOUT;
    }

    check(epoll_ctl(epfd, EPOLL_CTL_MOD, c->fd, &ev), "MOD");
}

static void accept_clients(int listener) {
    /* Accept new connections, assign client slots, and register their FDs. */
    for (;;) {
        int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC); /* KEY_ACCEPT */
        if (fd < 0 && errno == EINTR) {
            continue;
        }

        if (fd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }

        check(fd, "accept4");
        int slot;
        for (slot = 0; slot < CLIENTS && peers[slot].fd >= 0; ++slot) {
        }

        if (slot == CLIENTS) {
            close(fd);
            continue;
        }

        peers[slot].fd = fd;
        struct epoll_event ev = {.events = EPOLLIN | EPOLLRDHUP, .data.u32 = (uint32_t)(slot + 1)};
        check(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev), "ADD client"); /* KEY_ADD_CLIENT */
        printf("ACCEPT fd=%d slot=%d\n", fd, slot);
    }
}

int main(int argc, char **argv) {
    /* Set up the server, run the event loop, and clean up sockets and FDs. */
    if (argc != 4 || (strcmp(argv[1], "rescan") && strcmp(argv[1], "onepass"))) {
        fprintf(stderr, "Usage: %s rescan|onepass SOCKET_PATH REQUESTS_CSV\n", argv[0]);
        return 2;
    }

    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(argv[2]) >= sizeof address.sun_path) {
        fprintf(stderr, "Socket path too long\n");
        return 2;
    }

    strcpy(address.sun_path, argv[2]);
    rescan_mode = !strcmp(argv[1], "rescan");
    if (!load_log(argv[3], &log_data)) {
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 0; i < CLIENTS; ++i) {
        peers[i].fd = -1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    check(sigaction(SIGINT, &sa, NULL), "SIGINT");
    check(sigaction(SIGTERM, &sa, NULL), "SIGTERM");
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); /* KEY_UDS */
    check(listener, "socket");
    /* Refuse an existing path; never unlink somebody else's socket. */
    check(bind(listener, (struct sockaddr *)&address, sizeof address), "bind");
    check(listen(listener, CLIENTS), "listen");
    epfd = epoll_create1(EPOLL_CLOEXEC);
    check(epfd, "epoll_create1");
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = 0};
    check(epoll_ctl(epfd, EPOLL_CTL_ADD, listener, &ev), "ADD listener");
    printf("READY pid=%ld epfd=%d listener=%d mode=%s rows=%zu socket=%s\n", (long)getpid(), epfd,
           listener, argv[1], log_data.count, argv[2]);
    while (!stopping) {
        struct epoll_event events[CLIENTS + 1];
        int n = epoll_wait(epfd, events, CLIENTS + 1, 500); /* KEY_SERVER_WAIT */
        if (n < 0 && errno == EINTR) {
            continue;
        }

        check(n, "epoll_wait");
        for (int i = 0; i < n && !stopping; ++i) {
            if (!events[i].data.u32) {
                accept_clients(listener);
                continue;
            }

            int slot = (int)events[i].data.u32 - 1;
            struct client *c = &peers[slot];
            if (c->fd < 0) {
                continue;
            }

            uint32_t flags = events[i].events;
            /* No worker thread: read_requests and its handler finish here. */
            if ((flags & EPOLLIN) && !read_requests(c)) { /* KEY_DISPATCH */
                continue;
            }

            if (c->fd >= 0 && (flags & EPOLLOUT) && !flush_reply(c)) {
                continue;
            }

            if (c->fd >= 0 && (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))) {
                disconnect_client(c);
                continue;
            }

            if (c->fd >= 0) {
                watch_client(c, slot);
            }
        }
    }

    for (int i = 0; i < CLIENTS; ++i) {
        if (peers[i].fd >= 0) {
            disconnect_client(&peers[i]);
        }
    }

    close(listener);
    close(epfd);
    free(log_data.records);
    check(unlink(argv[2]), "unlink own socket");
    puts("STOP cleaned socket");
    return 0;
}
