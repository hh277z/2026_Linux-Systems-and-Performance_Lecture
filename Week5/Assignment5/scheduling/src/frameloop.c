/* frameloop: a periodic frame loop that has to live inside its CPU reservation (Level 1).
 *
 * usage: sudo ./frameloop [-p dl|fifo:P|other] [-g none|signal|budget] [-n frames] [-o out.csv]
 *                         [-P period_us] [-r runtime_us] [-w work_us] [-s spike_every] [-S spike_us] [-B budget_us] [-c cpu]
 *   -p  scheduling of the frame thread (default dl). dl uses sched_setattr(SCHED_DEADLINE, runtime, period)
 *   -g  what to do when a frame needs more CPU than the reservation (default none)
 *         none    always finish the frame, then sched_yield() until the next period
 *         signal  SCHED_FLAG_DL_OVERRUN: when SIGXCPU has arrived, drop the rest of the frame, then sched_yield()
 *         budget  check the frame's own CPU time at every chunk and cut the frame at budget_us
 *   fixed workload: period 16667 us, runtime 4000 us, normal frame 2500 us, every 10th frame (id % 10 == 9) 6000 us
 *
 * Every frame calls frame_begin(id) and frame_end(id, status). Level 1's bpftrace script attaches uprobes there.
 */
#include "a5.h"
#include <signal.h>

#define CHUNK_NS 100000ull /* work is done in 100 us chunks: the points where the loop can stop a frame */
#define ST_OK 0
#define ST_DROP 1          /* stopped by SIGXCPU */
#define ST_CUT 2           /* stopped by the frame's own CPU budget */

struct frame {
    uint64_t id, start, end, cpu, target, done;
    int status, sig, miss;
};

static volatile sig_atomic_t g_overrun;  /* set by the SIGXCPU handler */
static volatile sig_atomic_t g_sigcount;

/* uprobe anchors */
__attribute__((noinline)) void frame_begin(uint64_t id) { __asm__ volatile("" : : "r"(id) : "memory"); }
__attribute__((noinline)) void frame_end(uint64_t id, int status) { __asm__ volatile("" : : "r"(id), "r"(status) : "memory"); }

static void on_sigxcpu(int sig) {
    (void)sig;
    g_overrun = 1;      /* KEY only set a flag: the frame loop decides at its next chunk boundary */
    g_sigcount++;
}

static const char *st_name(int s) {
    return s == ST_DROP ? "DROP" : s == ST_CUT ? "CUT" : "OK";
}

int main(int argc, char **argv) {
    const char *pol = "dl", *mode = "none", *out = NULL;
    uint64_t period = 16667, runtime = 4000, work = 2500, spike = 6000, budget = 0;
    int spike_every = 10, nframes = 600, cpu = 0, opt;
    while ((opt = getopt(argc, argv, "p:g:n:o:P:r:w:s:S:B:c:h")) != -1) {
        switch (opt) {
        case 'p': pol = optarg; break;
        case 'g': mode = optarg; break;
        case 'n': nframes = atoi(optarg); break;
        case 'o': out = optarg; break;
        case 'P': period = strtoull(optarg, NULL, 10); break;
        case 'r': runtime = strtoull(optarg, NULL, 10); break;
        case 'w': work = strtoull(optarg, NULL, 10); break;
        case 's': spike_every = atoi(optarg); break;
        case 'S': spike = strtoull(optarg, NULL, 10); break;
        case 'B': budget = strtoull(optarg, NULL, 10); break;
        case 'c': cpu = atoi(optarg); break;
        default:
            fprintf(stderr, "usage: %s [-p dl|fifo:P|other] [-g none|signal|budget] [-n frames] [-o out.csv] ...\n", argv[0]);
            return 1;
        }
    }
    int m_signal = !strcmp(mode, "signal"), m_budget = !strcmp(mode, "budget");
    if (!m_signal && !m_budget && strcmp(mode, "none")) {
        fprintf(stderr, "-g none|signal|budget\n");
        return 1;
    }
    int is_dl = !strcmp(pol, "dl");
    if (m_signal && !is_dl) {
        fprintf(stderr, "-g signal needs -p dl (SIGXCPU comes from the deadline reservation)\n");
        return 1;
    }
    if (!budget) {
        budget = runtime > 500 ? runtime - 500 : runtime; /* margin for the loop's own overhead */
    }
    if (nframes < 1 || nframes > 100000) {
        return 1;
    }
    struct frame *fr = calloc((size_t)nframes, sizeof *fr);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigxcpu;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGXCPU, &sa, NULL); /* KEY default action of SIGXCPU kills the process */

    pthread_setname_np(pthread_self(), "frameloop");
    if (is_dl) {
        struct a5_sched_attr a = { .size = sizeof a, .sched_policy = SCHED_DEADLINE,
                                   .sched_flags = SCHED_FLAG_DL_OVERRUN, /* KEY ask for SIGXCPU on overrun */
                                   .sched_runtime = runtime * 1000, .sched_deadline = period * 1000,
                                   .sched_period = period * 1000 };
        if (a5_sched_setattr(&a)) { /* KEY CBS reservation of this thread */
            perror(errno == EBUSY ? "sched_setattr (admission control refused)" : "sched_setattr (root? unpinned?)");
            return 1;
        }
    } else {
        if (pin_self(cpu)) {
            perror("pin");
            return 1;
        }
        if (!strncmp(pol, "fifo:", 5)) {
            int rc = set_fifo(atoi(pol + 5));
            if (rc) {
                fprintf(stderr, "SCHED_FIFO: %s\n", strerror(rc));
                return 1;
            }
        } else if (strcmp(pol, "other")) {
            fprintf(stderr, "-p dl|fifo:P|other\n");
            return 1;
        }
    }
    printf("pid=%d tid=%ld policy=%s mode=%s period=%lluus runtime=%lluus work=%lluus spike=%lluus/every %d budget=%lluus frames=%d\n",
           (int)getpid(), gettid_l(), pol, mode, (unsigned long long)period, (unsigned long long)runtime,
           (unsigned long long)work, (unsigned long long)spike, spike_every, (unsigned long long)budget, nframes);
    fflush(stdout);

    uint64_t P = period * 1000, t0 = now_ns(), next_release = t0;
    for (int f = 0; f < nframes; f++) {
        struct frame *x = &fr[f];
        if (!is_dl) {
            if (now_ns() < next_release) {
                sleep_until_ns(next_release);
            }
        }
        x->id = (uint64_t)f;
        x->target = ((f % spike_every) == spike_every - 1 ? spike : work) * 1000;
        int sig0 = g_sigcount;
        uint64_t c0 = thread_cpu_ns();
        x->start = now_ns();
        frame_begin(x->id);
        if (m_signal) {
            g_overrun = 0;
        }
        uint64_t done = 0;
        x->status = ST_OK;
        while (done < x->target) {
            uint64_t step = x->target - done < CHUNK_NS ? x->target - done : CHUNK_NS;
            if (m_signal && g_overrun) {
                x->status = ST_DROP; /* KEY SIGXCPU seen: stop this frame here */
                break;
            }
            if (m_budget && thread_cpu_ns() - c0 + step > budget * 1000) {
                x->status = ST_CUT;  /* KEY stop before the reservation runs out: never throttled */
                break;
            }
            burn_cpu(step);
            done += step;
        }
        frame_end(x->id, x->status);
        x->end = now_ns();
        x->cpu = thread_cpu_ns() - c0;
        x->done = done;
        x->sig = g_sigcount - sig0;
        uint64_t release = is_dl ? x->start : next_release;
        x->miss = x->end > release + P;
        if (is_dl) {
            sched_yield(); /* KEY frame done: give back the rest of the runtime, sleep until the next period */
        } else {
            next_release += P;
            if (now_ns() > next_release + P) { /* far behind: skip whole periods instead of bursting */
                next_release += ((now_ns() - next_release) / P) * P;
            }
        }
    }
    uint64_t t1 = now_ns();

    int miss = 0, drop = 0, cut = 0, sigs = 0;
    uint64_t want = 0, got = 0, *resp = calloc((size_t)nframes, sizeof *resp);
    for (int f = 0; f < nframes; f++) {
        miss += fr[f].miss;
        drop += fr[f].status == ST_DROP;
        cut += fr[f].status == ST_CUT;
        sigs += fr[f].sig;
        want += fr[f].target;
        got += fr[f].done;
        resp[f] = fr[f].end - fr[f].start;
    }
    uint64_t p50 = pct_u64(resp, (size_t)nframes, 50), p99 = pct_u64(resp, (size_t)nframes, 99), mx = resp[nframes - 1];
    printf("frames=%d elapsed_ms=%.1f periods_used=%.1f miss=%d drop=%d cut=%d sigxcpu=%d work_done=%.1f%% "
           "frame_wall_us p50=%.0f p99=%.0f max=%.0f\n",
           nframes, (t1 - t0) / 1e6, (double)(t1 - t0) / (double)P, miss, drop, cut, sigs, 100.0 * (double)got / (double)want,
           p50 / 1e3, p99 / 1e3, mx / 1e3);
    if (out) {
        FILE *fo = fopen(out, "w");
        if (!fo) {
            perror(out);
            return 1;
        }
        fprintf(fo, "id,start_us,wall_us,cpu_us,target_us,done_us,status,sigxcpu,miss\n");
        for (int f = 0; f < nframes; f++) {
            struct frame *x = &fr[f];
            fprintf(fo, "%llu,%.1f,%.1f,%.1f,%.0f,%.0f,%s,%d,%d\n", (unsigned long long)x->id, (x->start - t0) / 1e3,
                    (x->end - x->start) / 1e3, x->cpu / 1e3, x->target / 1e3, x->done / 1e3, st_name(x->status), x->sig, x->miss);
        }
        fclose(fo);
        printf("per-frame log: %s\n", out);
    }
    return 0;
}
