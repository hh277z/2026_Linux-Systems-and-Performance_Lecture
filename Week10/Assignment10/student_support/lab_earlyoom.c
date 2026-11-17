#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "meminfo.h"
#include "lmk_policy.h"

int earlyoom_upstream_main(int, char **);
static volatile sig_atomic_t stopping;

static void stop(int s) {
    (void)s;
    stopping = 1;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static unsigned long long ticks(int pid) {
    char name[80], buf[4096];
    snprintf(name, sizeof name, "/proc/%d/stat", pid);
    FILE *f = fopen(name, "r");

    if (!f)
        return 0;

    if (!fgets(buf, sizeof buf, f)) {
        fclose(f);
        return 0;
    }

    fclose(f);
    char *s = strrchr(buf, ')');
    if (!s)
        return 0;

    s += 2;
    char *save = NULL, *word = strtok_r(s, " ", &save);

    for (int field = 3; word; field++, word = strtok_r(NULL, " ", &save))
        if (field == 22)
            return strtoull(word, NULL, 10);

    return 0;
}

static int member(int pid, const char *cg) {
    char name[80], buf[4096];
    snprintf(name, sizeof name, "/proc/%d/cgroup", pid);
    FILE *f = fopen(name, "r");

    if (!f)
        return 0;

    int ok = 0;
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\n")] = 0;

        if (!strncmp(buf, "0::", 3) && !strcmp(buf + 3, cg + strlen("/sys/fs/cgroup")))
            ok = 1;
    }

    fclose(f);
    return ok;
}

typedef struct {
    lab_candidate c;
    unsigned long long start;
    char cg[1024];
} record;

static int load(const char *path, record *r) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 022) ||
        st.st_size > 65536 || (st.st_uid != geteuid() && st.st_uid != 0)) {
        if (fd >= 0)
            close(fd);
        return -1;
    }

    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return -1;
    }

    int n = 0;
    char line[2048], role[32];
    while (fgets(line, sizeof line, f)) {
        record x = {0};
        int extra;

        if (sscanf(line, "%d %llu %31s %1023s %n", &x.c.pid, &x.start, role, x.cg, &extra) != 4) {
            fclose(f);
            return -1;
        }

        if (strcmp(role, "candidate"))
            continue;

        char *base = strrchr(x.cg, '/');
        if (x.c.pid <= 1 || x.c.pid == getpid() || x.c.pid == getppid() || !x.start ||
            strncmp(x.cg, "/sys/fs/cgroup/", 15) || !base || strncmp(base + 1, "lab10-", 6) ||
            strstr(x.cg, "..")) {
            fclose(f);
            return -1;
        }

        if (ticks(x.c.pid) != x.start || !member(x.c.pid, x.cg))
            continue;

        if (get_oom_score_adj(x.c.pid, &x.c.adj) || x.c.adj == -1000)
            continue;

        x.c.score = get_oom_score(x.c.pid);

        if (x.c.score < 0)
            continue;

        char name[80], buf[256];
        snprintf(name, sizeof name, "/proc/%d/status", x.c.pid);
        FILE *s = fopen(name, "r");

        if (!s)
            continue;

        while (fgets(buf, sizeof buf, s))
            if (sscanf(buf, "VmRSS: %ld", &x.c.rss_kib) == 1)
                break;

        fclose(s);

        if (n == 128) {
            fclose(f);
            return -1;
        }

        r[n++] = x;
    }

    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : n;
}

static int signal_record(record *r, int sig, int *pfd) {
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    if (*pfd < 0)
        *pfd = (int)syscall(SYS_pidfd_open, r->c.pid, 0);

    if (*pfd < 0)
        return -1;

    if (ticks(r->c.pid) != r->start || !member(r->c.pid, r->cg)) {
        errno = ESRCH;
        return -1;
    }

    return (int)syscall(SYS_pidfd_send_signal, *pfd, sig, NULL, 0);
#else
    (void)r;
    (void)sig;
    (void)pfd;
    errno = ENOSYS;
    return -1;
#endif
}

typedef struct {
    char kind[8];
    unsigned long long stall, window;
    int fd;
} trigger;

static int amount(meminfo_t m, double mem, double swap) {
    return m.MemAvailablePercent <= mem && (m.SwapTotalKiB == 0 || m.SwapFreePercent <= swap);
}

static int fresh(trigger *t, int n, unsigned long long *previous, double elapsed) {
    FILE *f = fopen("/proc/pressure/memory", "r");
    if (!f)
        return -1;

    char line[256];
    unsigned long long s = 0, u = 0;

    while (fgets(line, sizeof line, f)) {
        unsigned long long v;
        if (sscanf(line, "some %*s %*s %*s total=%llu", &v) == 1)
            s = v;

        if (sscanf(line, "full %*s %*s %*s total=%llu", &v) == 1)
            u = v;
    }

    fclose(f);
    int active = 0;

    for (int i = 0; i < n; i++) {
        unsigned long long v = !strcmp(t[i].kind, "some") ? s : u;
        if (v < previous[i])
            active = -1;
        else if (active >= 0 && v - previous[i] >= (double)t[i].stall * elapsed * 1e6 / t[i].window)
            active = 1;
        previous[i] = v;
    }

    return active;
}

static double number(const char *s, double lo, double hi) {
    char *e;
    errno = 0;
    double v = strtod(s, &e);

    if (errno || *e || e == s || !isfinite(v) || v < lo || v > hi) {
        fprintf(stderr, "invalid number %s\n", s);
        exit(2);
    }

    return v;
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--lab"))
        return earlyoom_upstream_main(argc, argv);

    const char *registry = NULL;
    double seconds = 60, grace = 1, cool = 1, mem = 10, killmem = 5, swap = 10, killswap = 5;
    int dry = 0, importance = 0, priority = 0;
    trigger tr[8];
    int nt = 0;

    for (int i = 2; i < argc; i++) {
        char *o = argv[i];
        if (!strcmp(o, "--dryrun"))
            dry = 1;
        else if (!strcmp(o, "-p"))
            priority = 1;
        else if (!strcmp(o, "--registry") && i + 1 < argc)
            registry = argv[++i];
        else if (!strcmp(o, "--seconds") && i + 1 < argc)
            seconds = number(argv[++i], .1, 600);
        else if (!strcmp(o, "--grace") && i + 1 < argc)
            grace = number(argv[++i], 0, 30);
        else if (!strcmp(o, "--cooldown") && i + 1 < argc)
            cool = number(argv[++i], .1, 30);
        else if (!strcmp(o, "--mem-percent") && i + 1 < argc)
            mem = number(argv[++i], 0, 99);
        else if (!strcmp(o, "--kill-percent") && i + 1 < argc)
            killmem = number(argv[++i], 0, 99);
        else if (!strcmp(o, "--swap-percent") && i + 1 < argc)
            swap = number(argv[++i], 0, 100);
        else if (!strcmp(o, "--swap-kill-percent") && i + 1 < argc)
            killswap = number(argv[++i], 0, 100);
        else if (!strcmp(o, "--levels") && i + 1 < argc) {
            if (strcmp(argv[++i], "900,700,200,0"))
                return 2;

            importance = 1;
        } else if (!strcmp(o, "--psi") && i + 3 < argc && nt < 8) {
            trigger *t = &tr[nt++];
            const char *k = argv[++i];

            if (strcmp(k, "some") && strcmp(k, "full"))
                return 2;

            snprintf(t->kind, sizeof t->kind, "%s", k);
            t->stall = (unsigned long long)number(argv[++i], 1, 10000000);
            t->window = (unsigned long long)number(argv[++i], 500000, 10000000);

            if (t->stall > t->window)
                return 2;

            t->fd = -1;
        } else {
            fprintf(stderr,
                    "lab options: --registry FILE --seconds SEC --dryrun -p --psi some|full STALL "
                    "WINDOW --levels 900,700,200,0 --grace SEC --cooldown SEC --mem-percent N "
                    "--kill-percent N --swap-percent N --swap-kill-percent "
                    "N\nunsupported/conflicting option: %s\n",
                    o);
            return 2;
        }
    }

    if (!registry || killmem > mem || killswap > swap) {
        fputs("registry and ordered thresholds required\n", stderr);
        return 2;
    }

    record rs[128];
    if (load(registry, rs) < 0) {
        perror("registry rejected");
        return 1;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    if (priority) {
        int n = setpriority(PRIO_PROCESS, 0, -20);
        FILE *f = fopen("/proc/self/oom_score_adj", "w");
        int a = f ? fprintf(f, "-100") : -1;

        if (f)
            fclose(f);
        printf("t=%.6f priority nice_rc=%d adj_write=%d\n", now(), n, a);
    }

    int ml = mlockall(MCL_CURRENT);
    printf("t=%.6f event=start scope=registry upstream=v1.9.0 mlock_rc=%d errno=%d dryrun=%d\n",
           now(), ml, ml ? errno : 0, dry);
    for (int i = 0; i < nt; i++) {
        char spec[100];
        snprintf(spec, sizeof spec, "%s %llu %llu", tr[i].kind, tr[i].stall, tr[i].window);
        tr[i].fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK | O_CLOEXEC);

        if (tr[i].fd < 0 || write(tr[i].fd, spec, strlen(spec) + 1) < 0) {
            printf("t=%.6f event=psi_fallback errno=%d trigger=%s\n", now(), errno, spec);
            if (tr[i].fd >= 0)
                close(tr[i].fd);
            tr[i].fd = -1;
        } else
            printf("t=%.6f event=registered trigger=%s\n", now(), spec);
    }

    struct pollfd pf[8];
    int np = 0;
    for (int i = 0; i < nt; i++)
        if (tr[i].fd >= 0)
            pf[np++] = (struct pollfd){tr[i].fd, POLLPRI, 0};
    /* Failed registrations do not become successful synthetic PSI events. */
    int valid = 0;
    trigger active[8];

    for (int i = 0; i < nt; i++)
        if (tr[i].fd >= 0)
            active[valid++] = tr[i];
    double window = 1;

    for (int i = 0; i < valid; i++)
        if (active[i].window / 1e6 > window)
            window = active[i].window / 1e6;
    unsigned long long prev[8] = {0};
    fresh(active, valid, prev, window);
    double end = now() + seconds, next = 0, last = now();
    int level = 0, clean = 0, persistent = 0;
    int floors[] = {900, 700, 200, 0};
    int status = 0;

    while (!stopping && now() < end) {
        poll(pf, (nfds_t)np, 100);
        int event = 0;
        for (int i = 0; i < np; i++) {
            if (pf[i].revents & POLLPRI)
                event = 1;

            if (pf[i].revents & (POLLERR | POLLNVAL)) {
                puts("PSI descriptor error; stopping");
                status = 1;
                stopping = 1;
            }
        }

        if (now() < next)
            continue;

        meminfo_t m = parse_meminfo();
        int low = amount(m, mem, swap), critical = amount(m, killmem, killswap);

        if (now() - last >= window) {
            int f = fresh(active, valid, prev, now() - last);
            last = now();
            persistent = f > 0;

            if (f == 0 && !low) {
                if (++clean >= 2) {
                    level = 0;
                    persistent = 0;
                }
            } else
                clean = 0;
        }

        int act = lab_should_act(low, event || persistent);
        if (act < 0) {
            puts("policy not implemented");
            status = 1;
            break;
        }

        if (!act)
            continue;

        int n = load(registry, rs);
        if (n < 0) {
            puts("registry failure; refusing to widen scope");
            status = 1;
            break;
        }

        lab_candidate cs[128];
        for (int i = 0; i < n; i++)
            cs[i] = rs[i].c;
        int k = lab_choose(cs, n, importance, floors[level]);

        if (k == -2) {
            puts("importance policy not implemented");
            status = 1;
            break;
        }

        if (k < 0) {
            if (importance && (low || persistent) && level < 3) {
                level++;
                printf("t=%.6f event=level floor=%d\n", now(), floors[level]);
            } else
                printf("t=%.6f event=no_candidate\n", now());
            next = now() + cool;
            continue;
        }

        printf("t=%.6f event=select actor=earlyoom trigger=%s floor=%d pid=%d adj=%d rss_KiB=%ld "
               "mem_available_KiB=%lld\n",
               now(),
               event        ? "psi"
               : persistent ? "psi_persistent"
                            : "meminfo",
               floors[level], rs[k].c.pid, rs[k].c.adj, rs[k].c.rss_kib, m.MemAvailableKiB);

        if (dry) {
            next = now() + cool;
            continue;
        }

        int fd = -1, sig = critical ? SIGKILL : SIGTERM;
        if (signal_record(&rs[k], sig, &fd)) {
            printf("t=%.6f event=signal_error errno=%d\n", now(), errno);
            if (fd >= 0)
                close(fd);
            next = now() + cool;
            continue;
        }

        printf("t=%.6f event=signal pid=%d signal=%d via=pidfd\n", now(), rs[k].c.pid, sig);
        struct pollfd exitfd = {fd, POLLIN, 0};
        int exited = 0;
        double until = now() + grace;

        while (!stopping && now() < until) {
            if (poll(&exitfd, 1, 50) > 0 && (exitfd.revents & POLLIN)) {
                exited = 1;
                break;
            }
        }

        if (!exited && sig == SIGTERM && !stopping) {
            meminfo_t cur = parse_meminfo();
            int pressured = amount(cur, mem, swap);
            if (valid) {
                int f = fresh(active, valid, prev, now() - last);
                last = now();
                pressured |= f > 0;
            }

            if (pressured) {
                if (!signal_record(&rs[k], SIGKILL, &fd))
                    printf("t=%.6f event=signal pid=%d signal=9 via=pidfd\n", now(), rs[k].c.pid);
            } else
                printf("t=%.6f event=cancel_escalation pid=%d\n", now(), rs[k].c.pid);
        }
#if defined(SYS_process_mrelease)
        if (!exited && sig == SIGKILL) {
            int rc = (int)syscall(SYS_process_mrelease, fd, 0);
            printf("t=%.6f event=process_mrelease rc=%d errno=%d\n", now(), rc, rc ? errno : 0);
        }
#endif
        if (!exited && poll(&exitfd, 1, 1000) > 0 && (exitfd.revents & POLLIN))
            exited = 1;
        printf("t=%.6f event=%s pid=%d\n", now(), exited ? "exit_observed" : "exit_pending",
               rs[k].c.pid);
        close(fd);
        next = now() + cool;
        /* No second victim while the previous registered identity is alive. */
        if (!exited) {
            while (!stopping && now() < end && ticks(rs[k].c.pid) == rs[k].start) {
                char p[80], s[4096];
                snprintf(p, sizeof p, "/proc/%d/stat", rs[k].c.pid);
                FILE *f = fopen(p, "r");
                int zombie = 0;

                if (f) {
                    if (fgets(s, sizeof s, f)) {
                        char *q = strrchr(s, ')');
                        zombie = q && q[2] == 'Z';
                    }

                    fclose(f);
                }

                if (zombie)
                    break;

                struct timespec wait = {0, 100000000};
                nanosleep(&wait, NULL);
            }
        }
    }

    for (int i = 0; i < nt; i++)
        if (tr[i].fd >= 0)
            close(tr[i].fd);
    munlockall();
    return status;
}
