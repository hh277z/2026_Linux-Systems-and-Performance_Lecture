/*
 * albumsvc.c - an album service with a UI thread (Level 2).
 *
 *   ./bin/albumsvc [-m lock|snapshot|handoff] [-d seconds] [-D disk_ms] [-Z compress_us]
 *                  [-S save_ms] [-c saver_cpu] [-o frames.csv]
 *
 * Threads
 *   ui       draws one frame every 16.7 ms: reads the whole album, then 2 ms of drawing work
 *   scan-0/1 "scan a file" (3 ms sleep), then change one album item
 *   saver    saves the album every save_ms
 *
 * Modes
 *   lock      album_lock protects the album. The saver holds it for the whole save. (provided)
 *   snapshot  the saver copies the album under the lock and saves the copy outside it. (TODO)
 *   handoff   the UI thread owns the album and nobody locks it. Workers send updates
 *             to the UI through a bq, and the UI sends snapshots to the saver. (TODO)
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "album.h"
#include "bq.h"
#include "common.h"

/* ==== configuration =================================================== */

enum mode { MODE_LOCK, MODE_SNAPSHOT, MODE_HANDOFF };
static const char *mode_name[] = { "lock", "snapshot", "handoff" };

static struct {
    enum mode mode;
    int seconds;            /* -d */
    int disk_ms;            /* -D  slow flash delay after fsync           */
    int compress_us;        /* -Z  CPU work of one save                    */
    int save_ms;            /* -S  save period                             */
    int saver_cpu;          /* -c  pin the saver (-1: do not pin)          */
    const char *csv;        /* -o  per-frame records                       */
    /* fixed parameters */
    int frame_us;           /* 16667: one frame at 60 FPS                  */
    int draw_us;            /* 2000: drawing work after reading the album  */
    int scan_us;            /* 3000: a worker "reads a file"               */
    int apply_budget;       /* handoff: at most this many updates per frame */
} cfg = {
    .mode = MODE_LOCK, .seconds = 10, .disk_ms = 20, .compress_us = 3000, .save_ms = 240,
    .saver_cpu = -1, .csv = NULL,
    .frame_us = 16667, .draw_us = 2000, .scan_us = 3000, .apply_budget = 64,
};

/* ==== shared state ==================================================== */

/* lock and snapshot modes: album_lock protects album.
 * handoff mode: the UI thread owns album, and no other thread touches it.
 * Locks, queues, and thread functions in this file are not static, so that
 * lockcheck can print their names (-rdynamic exports only non-static symbols). */
static album_t album;
pthread_mutex_t album_lock = PTHREAD_MUTEX_INITIALIZER;

static atomic_int stop;
static char save_path[64];

/* handoff mode */
enum { MSG_UPDATE = 1, MSG_SNAPSHOT = 2 };
bq_t ui_inbox;                      /* workers -> UI:  MSG_UPDATE,   ptr = album_update_t *        */
bq_t saver_inbox;                   /* UI -> saver:    MSG_SNAPSHOT, ptr = album_t * (a snapshot)   */
static MAYBE_UNUSED atomic_int save_in_flight;   /* 1 from just before the UI's put until the saver has freed the snapshot */

/* ==== measurements ==================================================== */

#define MAX_FRAMES 100000
#define MAX_SAVES  10000

static struct {
    uint64_t frame_late_us[MAX_FRAMES];    /* E_k - S_k                                  */
    uint64_t ui_call_us[MAX_FRAMES];       /* lock/snapshot: latency of the UI's pthread_mutex_lock call.
                                              handoff: time to apply queued updates   */
    int      frames, misses, skipped;
    uint64_t hold_us[MAX_SAVES];           /* how long the saver held album_lock (lock, snapshot) */
    uint64_t save_us[MAX_SAVES];           /* how long one album_save took                 */
    int      saves, holds;
    atomic_long updates_sent, updates_applied;
    uint64_t saver_cpu_ns;                 /* CPU time the saver thread used         */
    long     tid_ui, tid_saver, tid_scan[2];
} st;

static long gettid_long(void) { return (long)syscall(SYS_gettid); }

/* Saver-side records. Only the saver thread calls these. */
static void record_hold(uint64_t locked_ns, uint64_t unlocked_ns)
{
    if (st.holds < MAX_SAVES)
        st.hold_us[st.holds++] = (unlocked_ns - locked_ns) / 1000;
}

static void record_save(uint64_t start_ns, uint64_t end_ns)
{
    if (st.saves < MAX_SAVES)
        st.save_us[st.saves++] = (end_ns - start_ns) / 1000;
}

/* ==== scanner workers ================================================= */

/*
 * TODO(L2-3): handoff mode.
 *  Send u to the UI thread through ui_inbox (kind MSG_UPDATE) and do not
 *  touch u afterwards: it belongs to the UI now. If the queue is closed,
 *  nobody else will ever see u, so free it here.
 *  Returns 0 if u was handed off, -1 if the queue is closed.
 */
int worker_handoff_send(album_update_t *u)
{
    static atomic_int warned;
    if (!atomic_exchange(&warned, 1))
        fprintf(stderr, "TODO(L2-3) worker_handoff_send is not implemented\n");
    update_free(u);
    return -1;                      /* replace this stub */
}

void *worker_main(void *arg)
{
    int id = (int)(long)arg;
    char name[16];
    snprintf(name, sizeof name, "scan-%d", id);
    set_thread_name(name);
    st.tid_scan[id] = gettid_long();
    unsigned seed = 1234u + (unsigned)id;

    while (!atomic_load(&stop)) {
        sleep_us((uint64_t)cfg.scan_us);            /* "read a file": no lock held */
        album_update_t *u = update_make(&seed);
        if (!u)
            continue;

        if (cfg.mode == MODE_HANDOFF) {
            if (worker_handoff_send(u) != 0)
                break;
        } else {
            pthread_mutex_lock(&album_lock);
            album_apply(&album, u);                 /* short: swap a pointer, free the old title */
            pthread_mutex_unlock(&album_lock);
            update_free(u);
        }
        atomic_fetch_add(&st.updates_sent, 1);
    }
    return NULL;
}

/* ==== saver =========================================================== */

/* lock mode (provided): the whole save runs under album_lock. */
void saver_save_locked(void)
{
    pthread_mutex_lock(&album_lock);
    uint64_t locked = now_ns();
    album_save(&album, save_path, cfg.compress_us, cfg.disk_ms);
    uint64_t done = now_ns();
    pthread_mutex_unlock(&album_lock);
    record_hold(locked, done);
    record_save(locked, done);
}

/*
 * TODO(L2-2): snapshot mode.
 *  Hold album_lock only while album_snapshot copies the album. Save the copy
 *  (album_save) after unlocking, then free it. Record the time the lock was
 *  held with record_hold and the time of the save with record_save.
 */
void saver_save_snapshot(void)
{
    static int warned;
    if (!warned++)
        fprintf(stderr, "TODO(L2-2) saver_save_snapshot is not implemented\n");
}

/*
 * TODO(L2-5, saver side): handoff mode.
 *  Take snapshots from saver_inbox until bq_get returns BQ_CLOSED. Save each
 *  one (record_save), free it, then clear save_in_flight.
 */
void saver_handoff_loop(void)
{
    fprintf(stderr, "TODO(L2-5) saver_handoff_loop is not implemented\n");
}

void *saver_main(void *arg)
{
    (void)arg;
    set_thread_name("saver");
    st.tid_saver = gettid_long();
    if (cfg.saver_cpu >= 0 && pin_to_cpu(cfg.saver_cpu) != 0)
        fprintf(stderr, "saver: could not pin to CPU %d\n", cfg.saver_cpu);

    if (cfg.mode == MODE_HANDOFF) {
        saver_handoff_loop();
        st.saver_cpu_ns = thread_cpu_ns();
        return NULL;
    }
    uint64_t next = now_ns() + (uint64_t)cfg.save_ms * 1000000ull;
    while (!atomic_load(&stop)) {
        sleep_until_ns(next);
        next += (uint64_t)cfg.save_ms * 1000000ull;
        if (atomic_load(&stop))
            break;
        if (cfg.mode == MODE_LOCK)
            saver_save_locked();
        else
            saver_save_snapshot();
    }
    st.saver_cpu_ns = thread_cpu_ns();
    return NULL;
}

/* ==== UI ============================================================== */

/*
 * TODO(L2-4): handoff mode.
 *  Apply at most cfg.apply_budget queued updates without blocking
 *  (bq_timed_get with timeout 0). The UI thread owns the album, so no lock is
 *  needed. album_apply takes the title. Then free the update and count it in
 *  st.updates_applied.
 */
void ui_handoff_apply(void)
{
    static int warned;
    if (!warned++)
        fprintf(stderr, "TODO(L2-4) ui_handoff_apply is not implemented\n");
}

/*
 * TODO(L2-5, UI side): handoff mode.
 *  When now >= *next_save and no save is in flight:
 *   - set the next due time (*next_save = now + save period)
 *   - make a snapshot of the album (the UI owns it, so no lock)
 *   - set save_in_flight, then put the snapshot into saver_inbox (MSG_SNAPSHOT)
 *   - if the put fails (closed), the snapshot is still yours: free it and
 *     clear save_in_flight
 */
void ui_handoff_request_save(uint64_t now, uint64_t *next_save)
{
    static int warned;
    (void)now;
    (void)next_save;
    if (!warned++)
        fprintf(stderr, "TODO(L2-5) ui_handoff_request_save is not implemented\n");
}

/*
 * Frame k is scheduled to start at S_k = S_0 + k * frame_us and ends at E_k,
 * when its drawing work is done.
 *   frame_late = E_k - S_k
 *   miss       = E_k > S_k + frame_us (it was not ready when frame k + 1 should start)
 * After a miss the UI resumes at the next scheduled start that is still ahead
 * and counts the starts it passed as skipped.
 */
void *ui_main(void *arg)
{
    (void)arg;
    set_thread_name("ui");
    st.tid_ui = gettid_long();

    const uint64_t period = (uint64_t)cfg.frame_us * 1000ull;
    const uint64_t s0 = now_ns() + 100000000ull;              /* start in 100 ms */
    const uint64_t end = s0 + (uint64_t)cfg.seconds * 1000000000ull;
    uint64_t next_save = s0 + (uint64_t)cfg.save_ms * 1000000ull;
    uint64_t k = 0;
    volatile uint64_t checksum = 0;

    while (st.frames < MAX_FRAMES) {
        uint64_t sk = s0 + k * period;
        if (sk >= end)
            break;
        sleep_until_ns(sk);

        uint64_t t0 = now_ns(), t1;
        if (cfg.mode == MODE_HANDOFF) {
            ui_handoff_apply();
            t1 = now_ns();
            ui_handoff_request_save(t1, &next_save);
            checksum += album_draw(&album);
        } else {
            pthread_mutex_lock(&album_lock);          /* lock acquisition call latency: t1 - t0 */
            t1 = now_ns();
            checksum += album_draw(&album);
            pthread_mutex_unlock(&album_lock);
        }
        burn_cpu_us((uint64_t)cfg.draw_us);           /* drawing, outside the lock */
        uint64_t ek = now_ns();

        int i = st.frames++;
        st.frame_late_us[i] = (ek - sk) / 1000;
        st.ui_call_us[i] = (t1 - t0) / 1000;
        if (ek > sk + period)
            st.misses++;

        uint64_t next = k + 1;
        if (ek > s0 + next * period)                 /* passed one or more scheduled starts */
            next = (ek - s0) / period + 1;
        st.skipped += (int)(next - (k + 1));
        k = next;
    }
    (void)checksum;
    return NULL;
}

/* ==== reporting ======================================================= */

static void print_dist(const char *label, uint64_t *v, int n)
{
    if (n == 0) {
        printf("%-14s n=0\n", label);
        return;
    }
    uint64_t *c = malloc(sizeof *c * (size_t)n);
    memcpy(c, v, sizeof *c * (size_t)n);
    printf("%-14s n=%d p50=%llu p99=%llu max=%llu (us)\n", label, n,
           (unsigned long long)percentile(c, (size_t)n, 50),
           (unsigned long long)percentile(c, (size_t)n, 99),
           (unsigned long long)percentile(c, (size_t)n, 100));
    free(c);
}

static void write_csv(void)
{
    FILE *f = fopen(cfg.csv, "w");
    if (!f) {
        perror(cfg.csv);
        return;
    }
    fprintf(f, "frame,frame_late_us,%s,miss\n", cfg.mode == MODE_HANDOFF ? "ui_apply_us" : "ui_lock_us");
    for (int i = 0; i < st.frames; i++)
        fprintf(f, "%d,%llu,%llu,%d\n", i, (unsigned long long)st.frame_late_us[i],
                (unsigned long long)st.ui_call_us[i], st.frame_late_us[i] > (uint64_t)cfg.frame_us);
    fclose(f);
}

static void report(void)
{
    printf("frames=%d miss=%d skipped=%d\n", st.frames, st.misses, st.skipped);
    print_dist("frame_late", st.frame_late_us, st.frames);
    print_dist(cfg.mode == MODE_HANDOFF ? "ui_apply" : "ui_lock", st.ui_call_us, st.frames);
    if (cfg.mode != MODE_HANDOFF)
        print_dist("saver_hold", st.hold_us, st.holds);
    print_dist("save", st.save_us, st.saves);
    printf("saver_cpu_ms=%.1f\n", (double)st.saver_cpu_ns / 1e6);
    /* lock and snapshot: workers apply their own updates under album_lock.
     * handoff: the UI applies them, so the two numbers can differ at the end. */
    printf("updates_sent=%ld updates_applied=%ld\n", atomic_load(&st.updates_sent),
           cfg.mode == MODE_HANDOFF ? atomic_load(&st.updates_applied) : atomic_load(&st.updates_sent));
    if (cfg.csv)
        write_csv();
}

/* ==== main ============================================================ */

static void usage(void)
{
    fprintf(stderr, "usage: albumsvc [-m lock|snapshot|handoff] [-d seconds] [-D disk_ms] [-Z compress_us]\n"
                    "                [-S save_ms] [-c saver_cpu] [-o frames.csv]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int opt;
    setvbuf(stdout, NULL, _IOLBF, 0);
    while ((opt = getopt(argc, argv, "m:d:D:Z:S:c:o:")) != -1) {
        switch (opt) {
        case 'm':
            if (!strcmp(optarg, "lock")) cfg.mode = MODE_LOCK;
            else if (!strcmp(optarg, "snapshot")) cfg.mode = MODE_SNAPSHOT;
            else if (!strcmp(optarg, "handoff")) cfg.mode = MODE_HANDOFF;
            else usage();
            break;
        case 'd': cfg.seconds = atoi(optarg); break;
        case 'D': cfg.disk_ms = atoi(optarg); break;
        case 'Z': cfg.compress_us = atoi(optarg); break;
        case 'S': cfg.save_ms = atoi(optarg); break;
        case 'c': cfg.saver_cpu = atoi(optarg); break;
        case 'o': cfg.csv = optarg; break;
        default: usage();
        }
    }
    if (cfg.seconds < 1 || cfg.save_ms < 1)
        usage();

    snprintf(save_path, sizeof save_path, "/tmp/albumsvc-%d.dat", getpid());
    album_init(&album);
    if (cfg.mode == MODE_HANDOFF) {
        bq_init(&ui_inbox, 64, 0);
        bq_init(&saver_inbox, 1, 0);
    }

    printf("albumsvc: mode=%s seconds=%d disk_ms=%d compress_us=%d save_ms=%d saver_cpu=%d\n",
           mode_name[cfg.mode], cfg.seconds, cfg.disk_ms, cfg.compress_us, cfg.save_ms, cfg.saver_cpu);
    printf("pid=%d\n", getpid());

    pthread_t ui, saver, scan[2];
    pthread_create(&ui, NULL, ui_main, NULL);
    pthread_create(&saver, NULL, saver_main, NULL);
    for (long i = 0; i < 2; i++)
        pthread_create(&scan[i], NULL, worker_main, (void *)i);
    sleep_us(50000);
    printf("TIDS ui=%ld saver=%ld scan-0=%ld scan-1=%ld\n", st.tid_ui, st.tid_saver, st.tid_scan[0], st.tid_scan[1]);

    pthread_join(ui, NULL);                 /* the UI stops by itself after `seconds` */
    atomic_store(&stop, 1);
    if (cfg.mode == MODE_HANDOFF) {
        bq_close(&ui_inbox);                /* wakes workers blocked in bq_put   */
        bq_close(&saver_inbox);             /* lets the saver drain and return   */
    }
    for (int i = 0; i < 2; i++)
        pthread_join(scan[i], NULL);
    pthread_join(saver, NULL);

    if (cfg.mode == MODE_HANDOFF) {         /* updates nobody applied */
        bq_item_t it;
        while (bq_timed_get(&ui_inbox, &it, 0) == BQ_OK)
            update_free(it.ptr);
        bq_destroy(&ui_inbox);
        bq_destroy(&saver_inbox);
    }

    report();
    album_free(&album);
    unlink(save_path);
    return 0;
}
