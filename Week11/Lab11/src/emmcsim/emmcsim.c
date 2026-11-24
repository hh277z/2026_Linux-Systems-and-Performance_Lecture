/*
 * emmcsim.c: nbdkit plugin glue.
 *
 *   nbdkit -f -U /tmp/emmcsim.sock --threads 32 ./bin/emmcsim.so conf=conf/tv.conf [key=value ...]
 *   sudo nbd-client -unix /tmp/emmcsim.sock -b 4096 /dev/nbd0
 *
 * Keys on the command line override the ones in conf=. All times are in
 * microseconds and are multiplied by time_scale.
 *
 * Control socket (ctl=, default /tmp/emmcsim.ctl), used by bin/emmcctl:
 *   stat | stat raw | reset | trace | trace clear | powercut | conf
 */
#define _GNU_SOURCE
#define NBDKIT_API_VERSION 2
#include <nbdkit-plugin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "emmc.h"

#define THREAD_MODEL NBDKIT_THREAD_MODEL_PARALLEL
#ifndef NBDKIT_HANDLE_NOT_NEEDED                 /* older nbdkit */
#define NBDKIT_HANDLE_NOT_NEEDED ((void *)&dev_dead)
#endif

struct cfg cfg;
struct stats st;
pthread_mutex_t dev_lock = PTHREAD_MUTEX_INITIALIZER;
int dev_dead;

/* Values as written by the user, before time_scale (microseconds, KB, MB). */
static struct {
    double t_r, t_prog, t_bers, slc_t_r, slc_t_prog, slc_t_bers, t_cmd, phase;
    uint64_t page_kb, buffer_kb, pslc_mb;
} raw = {
    120, 1000, 3000,     /* TLC read (~120 us, p.41), program, and erase times */
    25, 200, 1500,       /* SLC read, program, and erase times */
    1,                   /* command processing time */
    40,                  /* suspension phase length */
    16, 16, 0,           /* 16 KB page, one 16 KB SRAM slot, no pSLC area */
};

static const char *suspend_name[] = { "off", "ips", "ipc" };

uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull +
           (uint64_t)ts.tv_nsec;
}

void sleep_until(uint64_t t)
{
    struct timespec ts = {
        (time_t)(t / 1000000000ull),
        (long)(t % 1000000000ull)
    };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

int region_of(uint64_t lba)
{
    return lba * BLK < cfg.pslc_bytes ? 1 : 0;
}

/* ---------- configuration ---------- */
static void defaults(void)
{
    cfg.size = 1ull << 30;
    cfg.dies = 2;
    /* Two dies with two planes each. */
    cfg.planes = 2;
    cfg.multiplane = 1;
    cfg.pages_per_block = 256;
    cfg.host_mbps = 400;
    /* HS400 bus */
    cfg.nand_mbps = 1600;
    /* 16 KB over the channel in 10 us */
    cfg.cache = 1;
    /* volatile cache is enabled */
    cfg.read_priority = 1;
    /* reads can pass queued programs */
    cfg.suspend = SUSP_OFF;
    cfg.erase_on_open = 0;
    cfg.time_scale = 1.0;
    strcpy(cfg.save, "/tmp/emmcsim.img");
    strcpy(cfg.ctl, "/tmp/emmcsim.ctl");
}

static int emmc_config(const char *key, const char *value);

static int load_conf(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        nbdkit_error("conf=%s: %m", path);
        return -1;
    }
    char line[512];
    int rc = 0;
    while (rc == 0 && fgets(line, sizeof line, f)) {
        char *h = strchr(line, '#');
        if (h)
            *h = 0;
        char *k = line;
        while (*k == ' ' || *k == '\t')
            k++;
        char *eq = strchr(k, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *v = eq + 1;
        char *e = k + strlen(k);
        while (e > k && (e[-1] == ' ' || e[-1] == '\t'))
            *--e = 0;
        while (*v == ' ' || *v == '\t')
            v++;
        e = v + strlen(v);
        while (e > v && (e[-1] == ' ' || e[-1] == '\t' ||
                         e[-1] == '\n' || e[-1] == '\r'))
            *--e = 0;
        if (*k)
            rc = emmc_config(k, v);
    }
    fclose(f);
    return rc;
}

static int emmc_config(const char *key, const char *value)
{
    char *end;
    double d = strtod(value, &end);
    int isnum = end != value && *end == 0;
#define NUM(name) \
    (strcmp(key, name) == 0 && \
     (isnum || (nbdkit_error("%s=%s: not a number", key, value), 0)))
    if (strcmp(key, "conf") == 0) {
        snprintf(cfg.conf, sizeof cfg.conf, "%s", value);
        return load_conf(value);
    } else if (strcmp(key, "size") == 0) {
        int64_t s = nbdkit_parse_size(value);
        if (s <= 0)
            return -1;
        cfg.size = (uint64_t)s;
    } else if (NUM("dies")) {
        cfg.dies = (int)d;
    } else if (NUM("planes")) {
        cfg.planes = (int)d;
    } else if (NUM("multiplane")) {
        cfg.multiplane = (int)d;
    } else if (NUM("page_kb")) {
        raw.page_kb = (uint64_t)d;
    } else if (NUM("pages_per_block")) {
        cfg.pages_per_block = (uint32_t)d;
    } else if (NUM("t_r_us")) {
        raw.t_r = d;
    } else if (NUM("t_prog_us")) {
        raw.t_prog = d;
    } else if (NUM("t_bers_us")) {
        raw.t_bers = d;
    } else if (NUM("slc_t_r_us")) {
        raw.slc_t_r = d;
    } else if (NUM("slc_t_prog_us")) {
        raw.slc_t_prog = d;
    } else if (NUM("slc_t_bers_us")) {
        raw.slc_t_bers = d;
    } else if (NUM("pslc_mb")) {
        raw.pslc_mb = (uint64_t)d;
    } else if (NUM("host_mbps")) {
        cfg.host_mbps = (uint32_t)d;
    } else if (NUM("nand_mbps")) {
        cfg.nand_mbps = (uint32_t)d;
    } else if (NUM("t_cmd_us")) {
        raw.t_cmd = d;
    } else if (NUM("buffer_kb")) {
        raw.buffer_kb = (uint64_t)d;
    } else if (NUM("cache")) {
        cfg.cache = (int)d;
    } else if (NUM("read_priority")) {
        cfg.read_priority = (int)d;
    } else if (NUM("suspend_phase_us")) {
        raw.phase = d;
    } else if (NUM("suspend_max")) {
        cfg.suspend_max = (uint32_t)d;
    } else if (NUM("erase_on_open")) {
        cfg.erase_on_open = (int)d;
    } else if (NUM("read_retry_pct")) {
        cfg.read_retry_pct = (uint32_t)d;
    } else if (NUM("time_scale")) {
        cfg.time_scale = d;
    } else if (NUM("trace")) {
        cfg.trace = (int)d;
    } else if (strcmp(key, "suspend") == 0) {
        int i;
        for (i = 0; i < 3 && strcasecmp(value, suspend_name[i]); i++) {
        }
        if (i == 3) {
            nbdkit_error("suspend=%s: use off, ips or ipc", value);
            return -1;
        }
        cfg.suspend = i;
    } else if (strcmp(key, "image") == 0) {
        snprintf(cfg.image, sizeof cfg.image, "%s", value);
    } else if (strcmp(key, "save") == 0) {
        snprintf(cfg.save, sizeof cfg.save, "%s", value);
    } else if (strcmp(key, "ctl") == 0) {
        snprintf(cfg.ctl, sizeof cfg.ctl, "%s", value);
    } else {
        nbdkit_error("unknown key %s", key);
        return -1;
    }
#undef NUM
    return 0;
}

static uint64_t us(double v)
{
    return (uint64_t)(v * 1000.0 * cfg.time_scale + 0.5);
}

static int emmc_config_complete(void)
{
    cfg.page = (uint32_t)(raw.page_kb * 1024);
    cfg.t_r[0] = us(raw.t_r);
    cfg.t_r[1] = us(raw.slc_t_r);
    cfg.t_prog[0] = us(raw.t_prog);
    cfg.t_prog[1] = us(raw.slc_t_prog);
    cfg.t_bers[0] = us(raw.t_bers);
    cfg.t_bers[1] = us(raw.slc_t_bers);
    cfg.t_cmd = us(raw.t_cmd);
    cfg.suspend_phase = us(raw.phase);
    cfg.pslc_bytes = raw.pslc_mb << 20;

    if (cfg.dies < 1 || cfg.dies > MAX_DIES) {
        nbdkit_error("dies must be 1..%d", MAX_DIES);
        return -1;
    }
    if (cfg.planes < 1 || cfg.planes > MAX_PLANES) {
        nbdkit_error("planes must be 1..%d", MAX_PLANES);
        return -1;
    }
    if (cfg.page < BLK || cfg.page % BLK) {
        nbdkit_error("page_kb must be a multiple of 4");
        return -1;
    }
    if (raw.buffer_kb * 1024 < cfg.page || (raw.buffer_kb * 1024) % cfg.page) {
        nbdkit_error("buffer_kb must be a multiple of page_kb");
        return -1;
    }
    cfg.buffer_slots = (uint32_t)(raw.buffer_kb * 1024 / cfg.page);
    if (cfg.size % cfg.page) {
        nbdkit_error("size must be a multiple of page_kb");
        return -1;
    }
    if (cfg.pslc_bytes > cfg.size || cfg.pslc_bytes % cfg.page) {
        nbdkit_error("pslc_mb too large");
        return -1;
    }
    if (!cfg.host_mbps || !cfg.nand_mbps || cfg.time_scale <= 0) {
        nbdkit_error("bandwidth and time_scale must be > 0");
        return -1;
    }
    if (cfg.suspend && !cfg.suspend_phase) {
        nbdkit_error("suspend_phase_us must be > 0");
        return -1;
    }

    if (store_init() || ftl_init() || model_init())
        return -1;
    if (cfg.image[0] && store_load(cfg.image) < 0) {
        nbdkit_error("image=%s: cannot load", cfg.image);
        return -1;
    }
    return 0;
}

static void start_ctl(void);
#include <nbdkit-version.h>
#if NBDKIT_VERSION_MAJOR > 1 || NBDKIT_VERSION_MINOR >= 30
#define HAVE_AFTER_FORK 1
/* nbdkit has finished setting up stdin/stdout and forking: safe to add a thread. */
static int emmc_after_fork(void)
{
    start_ctl();
    return 0;
}
#endif

static void print_conf(FILE *f)
{
    fprintf(f,
            "size=%lu\ndies=%d\nplanes=%d\nmultiplane=%d\n"
            "page_kb=%lu\npages_per_block=%u\n",
            (unsigned long)cfg.size, cfg.dies, cfg.planes, cfg.multiplane,
            (unsigned long)raw.page_kb, cfg.pages_per_block);
    fprintf(f,
            "t_r_us=%g\nt_prog_us=%g\nt_bers_us=%g\n"
            "slc_t_r_us=%g\nslc_t_prog_us=%g\nslc_t_bers_us=%g\n"
            "pslc_mb=%lu\n",
            raw.t_r, raw.t_prog, raw.t_bers, raw.slc_t_r,
            raw.slc_t_prog, raw.slc_t_bers,
            (unsigned long)raw.pslc_mb);
    fprintf(f,
            "host_mbps=%u\nnand_mbps=%u\nt_cmd_us=%g\n"
            "buffer_kb=%lu\ncache=%d\nread_priority=%d\n",
            cfg.host_mbps, cfg.nand_mbps, raw.t_cmd,
            (unsigned long)raw.buffer_kb, cfg.cache, cfg.read_priority);
    fprintf(f,
            "suspend=%s\nsuspend_phase_us=%g\nsuspend_max=%u\n"
            "erase_on_open=%d\nread_retry_pct=%u\ntime_scale=%g\n",
            suspend_name[cfg.suspend], raw.phase, cfg.suspend_max, cfg.erase_on_open,
            cfg.read_retry_pct, cfg.time_scale);
}

/* ---------- control socket ---------- */
static void handle(int fd)
{
    char cmd[128] = "";
    ssize_t n = read(fd, cmd, sizeof cmd - 1);
    if (n <= 0)
        return;
    cmd[n] = 0;
    cmd[strcspn(cmd, "\r\n")] = 0;
    FILE *f = fdopen(dup(fd), "w");
    if (!f)
        return;
    pthread_mutex_lock(&dev_lock);
    if (strcmp(cmd, "stat") == 0) {
        model_stat(f, 0);
    } else if (strcmp(cmd, "stat raw") == 0) {
        model_stat(f, 1);
    } else if (strcmp(cmd, "reset") == 0) {
        model_reset_stats();
        fprintf(f, "counters reset\n");
    } else if (strcmp(cmd, "trace") == 0) {
        model_trace(f, 0);
    } else if (strcmp(cmd, "trace clear") == 0) {
        model_trace(f, 1);
    } else if (strcmp(cmd, "conf") == 0) {
        print_conf(f);
    } else if (strcmp(cmd, "powercut") == 0) {
        if (dev_dead) {
            fprintf(f, "already powered off\n");
        } else {
            model_powercut();
            int rc = store_save(cfg.save);
            dev_dead = 1;
            fprintf(f,
                    rc ? "power cut, saving %s failed: %s\n"
                       : "power cut: data on NAND saved to %s\n",
                    cfg.save, rc ? strerror(-rc) : "");
        }
    } else {
        fprintf(f, "unknown command: %s\n", cmd);
    }
    pthread_mutex_unlock(&dev_lock);
    fclose(f);
}

static void *ctl_thread(void *arg)
{
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle(fd);
        close(fd);
    }
    return NULL;
}

/* The control thread must live in the process that serves requests, and must
 * not start while nbdkit is still reopening stdin/stdout on /dev/null.
 * Started in .after_fork (nbdkit >= 1.30), otherwise at the first .open. */
static pid_t ctl_pid;
static pthread_mutex_t ctl_mu = PTHREAD_MUTEX_INITIALIZER;

static void start_ctl(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", cfg.ctl);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(cfg.ctl);
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 8) < 0) {
        nbdkit_error("control socket %s: %m", cfg.ctl);
        return;
    }
    chmod(cfg.ctl, 0666);
    ctl_pid = getpid();
    pthread_t t;
    pthread_create(&t, NULL, ctl_thread, (void *)(intptr_t)ls);
    pthread_detach(t);
}

/* ---------- nbdkit callbacks ---------- */
static void low_timer_slack(void) /* Default slack would blur short transfers. */
{
    static __thread int done;
    if (!done) {
        prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
        done = 1;
    }
}

static void *emmc_open(int readonly)
{
    (void)readonly;
    pthread_mutex_lock(&ctl_mu);
    if (ctl_pid != getpid())
        start_ctl();
    pthread_mutex_unlock(&ctl_mu);
    return NBDKIT_HANDLE_NOT_NEEDED;
}

static int64_t emmc_get_size(void *h)
{
    (void)h;
    return (int64_t)cfg.size;
}

static int emmc_can_flush(void *h)
{
    (void)h;
    return cfg.cache;
}

static int emmc_can_fua(void *h)
{
    (void)h;
    return cfg.cache ? NBDKIT_FUA_NATIVE : NBDKIT_FUA_NONE;
}

static int emmc_can_trim(void *h)
{
    (void)h;
    return 1;
}

static int emmc_is_rotational(void *h)
{
    (void)h;
    return 0;
}

static int dead(void)
{
    nbdkit_set_error(EIO);
    return -1;
}

/* Wait until every program in w has ended. A suspended read can push a
 * program back, so check again after each wake-up. */
static int wait_programs(const struct waitref *w, int nw)
{
    for (;;) {
        pthread_mutex_lock(&dev_lock);
        uint64_t e = dev_dead ? 0 : model_wait_end(w, nw);
        int gone = dev_dead;
        pthread_mutex_unlock(&dev_lock);
        if (gone)
            return dead();
        if (!e)
            return 0;
        sleep_until(e);
    }
}

static int emmc_pread(void *h, void *buf, uint32_t count, uint64_t offset, uint32_t flags)
{
    (void)h;
    (void)flags;
    low_timer_slack();
    pthread_mutex_lock(&dev_lock);
    if (dev_dead) {
        pthread_mutex_unlock(&dev_lock);
        return dead();
    }
    uint64_t done = model_read(offset, count, buf);
    pthread_mutex_unlock(&dev_lock);
    sleep_until(done);
    return 0;
}

/* FLUSH, and writes that must reach NAND: latency is known only at the end. */
static int finish_sync(uint64_t t0, int kind, const struct waitref *w, int nw)
{
    int rc = wait_programs(w, nw);
    pthread_mutex_lock(&dev_lock);
    model_lat(kind, now_ns() - t0);
    pthread_mutex_unlock(&dev_lock);
    return rc;
}

static int emmc_pwrite(void *h, const void *buf, uint32_t count, uint64_t offset, uint32_t flags)
{
    (void)h;
    low_timer_slack();
    struct waitref w[MAX_WAIT];
    int nw = 0;
    int sync = (flags & NBDKIT_FLAG_FUA) || !cfg.cache;
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&dev_lock);
    if (dev_dead) {
        pthread_mutex_unlock(&dev_lock);
        return dead();
    }
    uint64_t done = model_write(offset, count, buf,
                                (flags & NBDKIT_FLAG_FUA) != 0, w, &nw);
    pthread_mutex_unlock(&dev_lock);
    sleep_until(done);
    return sync ? finish_sync(t0, LAT_WRITE, w, nw) : 0;
}

static int emmc_flush(void *h, uint32_t flags)
{
    (void)h;
    (void)flags;
    low_timer_slack();
    struct waitref w[MAX_WAIT];
    int nw = 0;
    pthread_mutex_lock(&dev_lock);
    if (dev_dead) {
        pthread_mutex_unlock(&dev_lock);
        return dead();
    }
    uint64_t t0 = now_ns();
    uint64_t done = model_flush(w, &nw);
    pthread_mutex_unlock(&dev_lock);
    sleep_until(done);
    return finish_sync(t0, LAT_FLUSH, w, nw);
}

/* This model records TRIM requests but does not reclaim pages. */
static int emmc_trim(void *h, uint32_t count, uint64_t offset, uint32_t flags)
{
    (void)h;
    (void)flags;
    pthread_mutex_lock(&dev_lock);
    if (dev_dead) {
        pthread_mutex_unlock(&dev_lock);
        return dead();
    }
    st.host_trims++;
    for (uint64_t b = (offset + BLK - 1) / BLK;
         (b + 1) * BLK <= offset + count;
         b++) {
        ftl_trim(b);
    }
    pthread_mutex_unlock(&dev_lock);
    return 0;
}

static void emmc_load(void)
{
    defaults();
}

static void emmc_unload(void)
{
    if (cfg.ctl[0])
        unlink(cfg.ctl);
}

static void emmc_dump_plugin(void)
{
    print_conf(stdout);
}

static struct nbdkit_plugin plugin = {
    .name             = "emmcsim",
    .version          = "11.1",
    .longname         = "eMMC timing model for the Week 11 lab",
    .config_help      = "conf=FILE and key=value (see conf/tv.conf)",
    .load             = emmc_load,
    .unload           = emmc_unload,
    .config           = emmc_config,
    .config_complete  = emmc_config_complete,
#ifdef HAVE_AFTER_FORK
    .after_fork       = emmc_after_fork,
#endif
    .dump_plugin      = emmc_dump_plugin,
    .open             = emmc_open,
    .get_size         = emmc_get_size,
    .can_flush        = emmc_can_flush,
    .can_fua          = emmc_can_fua,
    .can_trim         = emmc_can_trim,
    .is_rotational    = emmc_is_rotational,
    .pread            = emmc_pread,
    .pwrite           = emmc_pwrite,
    .flush            = emmc_flush,
    .trim             = emmc_trim,
};

NBDKIT_REGISTER_PLUGIN(plugin)
