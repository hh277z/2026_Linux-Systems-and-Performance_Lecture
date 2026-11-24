/*
 * model.c: when does each request finish?
 *
 * Resources:
 *   host bus      one 8-bit half-duplex eMMC bus, shared by reads and writes
 *   NAND channel  one channel from the controller to the dies
 *   dies          each die runs one operation at a time (read, program, erase)
 *
 * Every request thread takes dev_lock, reserves time on the resources it
 * needs, and gets back the time its request completes. It then sleeps until
 * that time (sleep_until in emmcsim.c). There is no background thread: work
 * that "happens later" (a program, an erase) is reserved now, in the future.
 *
 * A die keeps its reservations in a list ordered by time. Reads can go ahead
 * of queued programs (read_priority) and can pause a running program
 * (suspend=ips|ipc). Erase operations are not suspended. When a program is
 * paused, the later reservations move back.
 *
 * Writes:
 *   host bus -> SRAM buffer slot (16 KB) -> sealed page -> NAND channel ->
 *   die page register -> program (with the other planes when multiplane=1)
 * A write completes when its data is in SRAM. If no slot is free the write waits.
 * Data becomes durable only when its program ends. FLUSH and FUA seal the
 * partial page (padding) and wait for every program to end.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "emmc.h"

struct op {
    uint64_t id, start, end;
    uint64_t gap;
    /* time this op was paused by suspensions */
    uint8_t kind;
    uint16_t nsusp;
    struct op *next;
};

struct page {                      /* one NAND page on its way to a die */
    uint64_t op;
    /* program op id, 0 = waiting for the other planes */
    uint64_t xfer_end;
    /* data is in the die's page register */
    int die, region;
    uint32_t fill;
    struct ver *vers, *last;
    struct page *next;
};

struct die {
    struct op *head, *tail;
    /* reservations, ordered by time */
    struct op *susp_op;
    /* op paused by the current suspension window */
    uint64_t susp_until;
    uint64_t prog_end;
    /* end of the last program: page register free */
    struct page *wait[NREGION][MAX_PLANES];
    int nwait[NREGION];
};

struct slot {
    struct page *pg;
    uint64_t free_at;
};

/* A bus or channel: reserved intervals, ordered by time. A transfer takes the
 * first gap that is long enough, so a command that arrives now does not wait
 * for a data transfer already reserved later. */
struct iv {
    uint64_t s, e;
    struct iv *next;
};

struct res {
    struct iv *head;
};

static struct die die[MAX_DIES];
static struct slot *slot[NREGION];
static uint32_t cur_slot[NREGION];
static struct res bus, chan;
static uint64_t op_seq;
static struct page *inflight, *inflight_tail;
/* sealed pages, not durable yet */
static uint64_t rng = 0x9E3779B97F4A7C15ull;

/* ---------- trace: finished die operations and host requests ---------- */
enum { EV_HREAD = 10, EV_HWRITE, EV_HFLUSH };
struct ev {
    uint64_t t0, t1;
    int16_t die;
    uint8_t kind;
    uint32_t aux;
};
#define TRACE_N (1u << 17)
static struct ev *trace;
static uint64_t trace_n, trace_t0;

static void trace_add(int kind, int d, uint64_t t0, uint64_t t1, uint32_t aux)
{
    if (!trace || trace_n >= TRACE_N)
        return;
    trace[trace_n++] = (struct ev){
        t0, t1, (int16_t)d, (uint8_t)kind, aux
    };
}

/* ---------- small helpers ---------- */
static uint64_t max64(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

static uint64_t xfer_ns(uint64_t bytes, uint32_t mbps)  /* 1 byte at 1 MB/s = 1000 ns */
{
    return (uint64_t)((double)bytes * 1000.0 / mbps * cfg.time_scale + 0.5);
}

static uint64_t res_reserve(struct res *r, uint64_t t, uint64_t d)
{
    struct iv **pp = &r->head;
    while (*pp && (*pp)->e <= t)
        pp = &(*pp)->next;
    while (*pp && (*pp)->s < t + d) {
        /* The interval overlaps; try after it. */
        t = (*pp)->e;
        pp = &(*pp)->next;
    }
    struct iv *prev_next = *pp;
    struct iv *n = malloc(sizeof *n);
    n->s = t;
    n->e = t + d;
    n->next = prev_next;
    *pp = n;
    return t + d;
}

static void res_prune(struct res *r, uint64_t now)
{
    while (r->head && r->head->e <= now) {
        struct iv *n = r->head->next;
        free(r->head);
        r->head = n;
    }
}

static uint64_t bus_xfer(uint64_t t, uint64_t ns)
{
    st.bus_busy += ns;
    return res_reserve(&bus, t, ns);
}

static uint64_t chan_xfer(uint64_t t, uint64_t bytes)
{
    uint64_t ns = xfer_ns(bytes, cfg.nand_mbps);
    st.chan_busy += ns;
    return res_reserve(&chan, t, ns);
}

static uint32_t rand32(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 32);
}

/* ---------- die reservations ---------- */
static struct op *op_new(int kind, uint64_t start, uint64_t dur)
{
    struct op *o = calloc(1, sizeof *o);
    o->id = ++op_seq;
    o->kind = (uint8_t)kind;
    o->start = start;
    o->end = start + dur;
    return o;
}

/* End time of op id on die d, or 0 if it already finished and was removed. */
static uint64_t op_end(int d, uint64_t id)
{
    for (struct op *o = die[d].head; o; o = o->next) {
        if (o->id == id)
            return o->end;
    }
    return 0;
}

/* Ops after a moved op start no earlier than t. Order never changes. */
static void push_back_from(struct op *o, uint64_t t)
{
    for (; o; o = o->next) {
        if (o->start >= t) {
            break;
        }
        uint64_t d = t - o->start;
        o->start += d;
        o->end += d;
        t = o->end;
    }
}

static struct op *die_append(int d, int kind, uint64_t ready, uint64_t dur)
{
    struct die *D = &die[d];
    struct op *o = op_new(kind, max64(ready, D->tail ? D->tail->end : 0), dur);
    if (D->tail)
        D->tail->next = o;
    else
        D->head = o;
    D->tail = o;
    st.die_busy[d][kind] += dur;
    return o;
}

static void insert_after(struct die *D, struct op *prev, struct op *o)
{
    if (prev) {
        o->next = prev->next;
        prev->next = o;
    } else {
        o->next = D->head;
        D->head = o;
    }
    if (D->tail == prev)
        D->tail = o;
}

/* Pause the running program o so that a read of dur can run.
 * Returns the read's end time, or 0 if this op may not be paused. */
static uint64_t try_suspend(int d, struct op *o, uint64_t ready, uint64_t dur)
{
    struct die *D = &die[d];
    uint64_t phase = cfg.suspend_phase, rs, extra;

        if (D->susp_op == o && ready < D->susp_until) {
            /* Join the open window. */
        rs = D->susp_until;
        extra = dur;
    } else {
        if (cfg.suspend_max && o->nsusp >= cfg.suspend_max)
            return 0;
        uint64_t done = ready - o->start - o->gap;
        if (cfg.suspend == SUSP_IPS) {
            /* Stop at the next phase boundary. */
            uint64_t boundary = (done + phase - 1) / phase * phase;
            rs = o->start + o->gap + boundary;
            if (rs >= o->end)
                return 0;
            extra = dur;
        } else {
            /* IPC stops now and repeats the interrupted phase work. */
            uint64_t lost = done % phase;
            rs = ready;
            extra = dur + lost;
            st.die_redo[d] += lost;
        }
        o->nsusp++;
        st.die_susp[d]++;
    }
    o->end += extra;
    o->gap += extra;
    D->susp_op = o;
    D->susp_until = rs + dur;
    push_back_from(o->next, o->end);
    st.die_busy[d][OP_READ] += dur;
    trace_add(OP_READ, d, rs, rs + dur, 1);
    return rs + dur;
}

/* Reserve a page read of dur on die d, ready at time ready. Returns its end. */
static uint64_t die_read(int d, uint64_t ready, uint64_t dur)
{
    struct die *D = &die[d];
    if (!cfg.read_priority) {
        struct op *o = die_append(d, OP_READ, ready, dur);
        return o->end;
    }
    struct op *prev = NULL, *cur = D->head;
    while (cur && cur->end <= ready) {
        prev = cur;
        cur = cur->next;
    }
    if (cur && cur->start <= ready) {
        /* An operation is already running. */
        if (cur->kind == OP_PROG && cfg.suspend != SUSP_OFF) {
            uint64_t e = try_suspend(d, cur, ready, dur);
            if (e)
                return e;
        }
        prev = cur;
        cur = cur->next;
    }
    while (cur && cur->kind == OP_READ) {
        prev = cur;
        cur = cur->next;
    }
    struct op *o = op_new(OP_READ, max64(ready, prev ? prev->end : 0), dur);
    insert_after(D, prev, o);
    push_back_from(o->next, o->end);
    st.die_busy[d][OP_READ] += dur;
    return o->end;
}

/* ---------- pages: SRAM buffer -> die ---------- */
static void die_program(int d, int r)
{
    struct die *D = &die[d];
    int n = D->nwait[r];
    uint64_t ready = 0;
    for (int i = 0; i < n; i++)
        ready = max64(ready, D->wait[r][i]->xfer_end);
    struct op *o = die_append(d, OP_PROG, ready, cfg.t_prog[r]);
    for (int i = 0; i < n; i++)
        D->wait[r][i]->op = o->id;
    D->prog_end = o->end;
    D->nwait[r] = 0;
    st.prog_ops++;
    st.pages_programmed += (uint64_t)n;
}

/* The page in slot s is full (or a flush needs it): send it to a die. */
static void seal(int r, struct slot *s, uint64_t t, int flushing)
{
    struct page *pg = s->pg;
    s->pg = NULL;
    st.pad_bytes += cfg.page - pg->fill;

    struct ppa a = ftl_alloc(r);
    struct die *D = &die[a.die];
    if (a.need_erase) {
        die_append(a.die, OP_ERASE, t, cfg.t_bers[r]);
        st.erases++;
    }
    pg->die = a.die;
    pg->region = r;
    pg->xfer_end = chan_xfer(max64(t, D->prog_end), cfg.page);
    s->free_at = pg->xfer_end;
    /* The slot is free after data leaves SRAM. */

    if (inflight_tail)
        inflight_tail->next = pg;
    else
        inflight = pg;
    inflight_tail = pg;

    D->wait[r][D->nwait[r]++] = pg;
    if (!cfg.multiplane || cfg.planes == 1 || flushing || D->nwait[r] == cfg.planes)
        die_program(a.die, r);
}

/* ---------- housekeeping: finished programs become durable ---------- */
static void commit_done(uint64_t now)
{
    struct page **pp = &inflight, *prev = NULL;
    while (*pp) {
        struct page *pg = *pp;
        uint64_t e = pg->op ? op_end(pg->die, pg->op) : UINT64_MAX;
        if (pg->op && e <= now) {
            /* An absent operation (e == 0) has already finished. */
            for (struct ver *v = pg->vers, *n; v; v = n) {
                n = v->next;
                store_commit(v, pg->die);
            }
            *pp = pg->next;
            if (inflight_tail == pg)
                inflight_tail = prev;
            free(pg);
        } else {
            prev = pg;
            pp = &pg->next;
        }
    }
}

static void prune(uint64_t now)
{
    for (int d = 0; d < cfg.dies; d++) {
        struct die *D = &die[d];
        while (D->head && D->head->end <= now) {
            struct op *o = D->head;
            trace_add(o->kind, d, o->start, o->end, o->nsusp);
            D->head = o->next;
            if (D->tail == o)
                D->tail = NULL;
            if (D->susp_op == o)
                D->susp_op = NULL;
            free(o);
        }
    }
}

static void housekeeping(uint64_t now)
{
    commit_done(now);
    prune(now);
    res_prune(&bus, now);
    res_prune(&chan, now);
}

/* Seal partial pages, start programs that wait for a partner plane, and
 * return, per die, the last program to wait for. */
static uint64_t flush_locked(uint64_t t, struct waitref *w, int *nw)
{
    for (int r = 0; r < NREGION; r++) {
        struct slot *s = &slot[r][cur_slot[r]];
        if (s->pg && s->pg->fill) {
            seal(r, s, t, 1);
            cur_slot[r] = (cur_slot[r] + 1) % cfg.buffer_slots;
        }
    }
    for (int d = 0; d < cfg.dies; d++) {
        for (int r = 0; r < NREGION; r++) {
            if (die[d].nwait[r])
                die_program(d, r);
        }
    }

    uint64_t last[MAX_DIES] = {0}, end = t;
    /* programs on a die finish in order */
    for (struct page *pg = inflight; pg; pg = pg->next) {
        if (pg->op > last[pg->die])
            last[pg->die] = pg->op;
    }
    *nw = 0;
    for (int d = 0; d < cfg.dies; d++)
        if (last[d]) {
            w[(*nw)++] = (struct waitref){ d, last[d] };
            end = max64(end, op_end(d, last[d]));
        }
    return end;
}

/* ---------- requests ---------- */
int model_init(void)
{
    for (int r = 0; r < NREGION; r++) {
        slot[r] = calloc(cfg.buffer_slots, sizeof **slot);
        if (!slot[r])
            return -1;
    }
    trace = calloc(TRACE_N, sizeof *trace);
    model_reset_stats();
    return trace ? 0 : -1;
}

uint64_t model_read(uint64_t off, uint32_t len, void *buf)
{
    uint64_t now = now_ns();
    housekeeping(now);
    uint64_t t = bus_xfer(now, cfg.t_cmd), ready = t;
    uint64_t first = off / BLK, last = (off + len - 1) / BLK;
    int gdie = -1;
    uint64_t gkey = 0, gbytes = 0, gdur = 0;

    for (uint64_t b = first; b <= last + 1; b++) {
        int pending = 0, old = 0, d = -1;
        uint64_t key = 0;
        const uint8_t *src = NULL;
        if (b <= last) {
            src = store_latest(b, &pending, &old);
            uint32_t boff = b == first ? (uint32_t)(off % BLK) : 0;
            uint32_t bend = b == last ? (uint32_t)((off + len - 1) % BLK) + 1 : BLK;
            memcpy((uint8_t *)buf + (b * BLK + boff - off), src + boff, bend - boff);
            if (pending) {
                st.buffer_hits++;
                continue;
                /* The data is in SRAM, so no NAND read is needed. */
            }
            d = ftl_lookup(b);
            key = b * BLK / cfg.page;
        }
        if (gdie >= 0 && (b > last || d != gdie || key != gkey)) {
            /* Finish the previous NAND page. */
            uint64_t re = die_read(gdie, t, gdur);
            ready = max64(ready, chan_xfer(re, gbytes));
            st.page_reads++;
            gdie = -1;
        }
        if (b > last)
            break;
        if (gdie < 0) {
            int r = region_of(b);
            gdie = d;
            gkey = key;
            gbytes = 0;
            gdur = cfg.t_r[r];
            if (old && cfg.read_retry_pct && rand32() % 100 < cfg.read_retry_pct) {
                gdur += cfg.t_r[r];
                /* One additional read attempt. */
                st.retries++;
            }
        }
        gbytes += BLK;
    }
    uint64_t done = bus_xfer(ready, xfer_ns(len, cfg.host_mbps));
    st.host_reads++;
    st.bytes_read += len;
    model_lat(LAT_READ, done - now);
    trace_add(EV_HREAD, -1, now, done, len);
    return done;
}

uint64_t model_write(uint64_t off, uint32_t len, const void *buf, int fua,
                     struct waitref *w, int *nw)
{
    uint64_t now = now_ns();
    housekeeping(now);
    uint64_t t = bus_xfer(now, cfg.t_cmd);
    const uint8_t *p = buf;
    *nw = 0;

    while (len) {
        uint64_t lba = off / BLK;
        uint32_t boff = (uint32_t)(off % BLK);
        uint32_t n = BLK - boff < len ? BLK - boff : len;
        int r = region_of(lba);
        struct slot *s = &slot[r][cur_slot[r]];
        if (s->free_at > t) {
            /* The SRAM buffer is full; wait for a free slot. */
            st.slot_stall += s->free_at - t;
            t = s->free_at;
        }
        t = bus_xfer(t, xfer_ns(n, cfg.host_mbps));
        if (!s->pg)
            s->pg = calloc(1, sizeof *s->pg);
        struct ver *v = store_new_version(lba, p, boff, n);
        if (s->pg->last)
            s->pg->last->next = v;
        else
            s->pg->vers = v;
        s->pg->last = v;
        s->pg->fill += BLK;
        if (s->pg->fill >= cfg.page) {
            seal(r, s, t, 0);
            cur_slot[r] = (cur_slot[r] + 1) % cfg.buffer_slots;
        }
        off += n;
        p += n;
        len -= n;
        st.bytes_written += n;
    }
    st.host_writes++;
    uint64_t done = t;
    if (fua || !cfg.cache) {
        if (fua)
            st.host_fua++;
        done = flush_locked(t, w, nw);
    } else {
        model_lat(LAT_WRITE, done - now);
    }
    trace_add(EV_HWRITE, -1, now, done, (uint32_t)(p - (const uint8_t *)buf));
    return done;
}

uint64_t model_flush(struct waitref *w, int *nw)
{
    uint64_t now = now_ns();
    housekeeping(now);
    uint64_t t = bus_xfer(now, cfg.t_cmd);
    st.host_flushes++;
    uint64_t done = flush_locked(t, w, nw);
    trace_add(EV_HFLUSH, -1, now, done, 0);
    return done;
}

/* Latest end among the ops in w, or 0 if all have finished. Programs can be
 * pushed back by suspended reads, so a waiter calls this again after waking. */
uint64_t model_wait_end(const struct waitref *w, int nw)
{
    uint64_t now = now_ns(), end = 0;
    housekeeping(now);
    for (int i = 0; i < nw; i++) {
        uint64_t e = op_end(w[i].die, w[i].op);
        if (e > now)
            end = max64(end, e);
    }
    return end;
}

/* Power is cut now: everything not yet on NAND is lost. */
void model_powercut(void)
{
    uint64_t now = now_ns();
    commit_done(now);
    /* Completed programs survive the power cut. */
    for (struct page *pg = inflight, *n; pg; pg = n) {
        n = pg->next;
        for (struct ver *v = pg->vers, *vn; v; v = vn) {
            vn = v->next;
            free(v);
        }
        free(pg);
    }
    inflight = inflight_tail = NULL;
    for (int r = 0; r < NREGION; r++)
        for (uint32_t i = 0; i < cfg.buffer_slots; i++)
            if (slot[r][i].pg) {
                for (struct ver *v = slot[r][i].pg->vers, *vn; v; v = vn) {
                    vn = v->next;
                    free(v);
                }
                free(slot[r][i].pg);
                slot[r][i].pg = NULL;
            }
    for (int d = 0; d < cfg.dies; d++) {
        for (struct op *o = die[d].head, *n; o; o = n) {
            n = o->next;
            free(o);
        }
        memset(&die[d], 0, sizeof die[d]);
    }
    res_prune(&bus, UINT64_MAX);
    res_prune(&chan, UINT64_MAX);
    store_drop_all_pending();
}

/* ---------- stat and trace output ---------- */
#define LAT_FINE 8192
static uint64_t lat_index(uint64_t us)
{
    return us < LAT_FINE ? us : LAT_FINE + (us - LAT_FINE) / 64;
}

static uint64_t lat_value(uint64_t i)
{
    return i < LAT_FINE ? i : LAT_FINE + (i - LAT_FINE) * 64;
}

void model_lat(int kind, uint64_t ns)
{
    uint64_t i = lat_index(ns / 1000);
    st.lat[kind][i < LAT_N ? i : LAT_N - 1]++;
}

/* p-th percentile of a latency histogram, in us */
static uint64_t lat_pct(const uint32_t *h, double p, uint64_t *count)
{
    uint64_t n = 0, acc = 0;
    for (int i = 0; i < LAT_N; i++)
        n += h[i];
    *count = n;
    if (!n)
        return 0;
    uint64_t want = (uint64_t)(p / 100.0 * n + 0.5);
    if (want < 1)
        want = 1;
    for (int i = 0; i < LAT_N; i++) {
        acc += h[i];
        if (acc >= want)
            return lat_value((uint64_t)i);
    }
    return lat_value(LAT_N - 1);
}

void model_reset_stats(void)
{
    memset(&st, 0, sizeof st);
    st.t_reset = now_ns();
    trace_n = 0;
    trace_t0 = st.t_reset;
}

static double pct(uint64_t part, uint64_t whole)
{
    return whole ? 100.0 * part / whole : 0;
}

void model_stat(FILE *f, int raw)
{
    uint64_t now = now_ns();
    housekeeping(now);
    uint64_t el = now - st.t_reset;
    uint64_t nand_bytes = st.pages_programmed * cfg.page;
    if (raw) {
        fprintf(f, "elapsed_ns=%lu\ntime_scale=%g\n", (unsigned long)el, cfg.time_scale);
        fprintf(f, "host_reads=%lu\nhost_writes=%lu\nhost_flushes=%lu\nhost_fua=%lu\nhost_trims=%lu\n",
                (unsigned long)st.host_reads, (unsigned long)st.host_writes,
                (unsigned long)st.host_flushes, (unsigned long)st.host_fua,
                (unsigned long)st.host_trims);
        fprintf(f, "bytes_read=%lu\nbytes_written=%lu\n",
                (unsigned long)st.bytes_read, (unsigned long)st.bytes_written);
        fprintf(f, "page_reads=%lu\nbuffer_hits=%lu\nretries=%lu\n",
                (unsigned long)st.page_reads, (unsigned long)st.buffer_hits, (unsigned long)st.retries);
        fprintf(f, "prog_ops=%lu\npages_programmed=%lu\nnand_bytes=%lu\npad_bytes=%lu\nerases=%lu\n",
                (unsigned long)st.prog_ops,
                (unsigned long)st.pages_programmed,
                (unsigned long)nand_bytes,
                (unsigned long)st.pad_bytes, (unsigned long)st.erases);
        for (int d = 0; d < cfg.dies; d++)
            fprintf(f,
                    "die%d_read_ns=%lu\ndie%d_prog_ns=%lu\n"
                    "die%d_erase_ns=%lu\ndie%d_redo_ns=%lu\n"
                    "die%d_susp=%lu\n",
                    d, (unsigned long)st.die_busy[d][OP_READ], d, (unsigned long)st.die_busy[d][OP_PROG],
                    d, (unsigned long)st.die_busy[d][OP_ERASE], d, (unsigned long)st.die_redo[d],
                    d, (unsigned long)st.die_susp[d]);
        fprintf(f, "bus_busy_ns=%lu\nchan_busy_ns=%lu\nslot_stall_ns=%lu\n",
                (unsigned long)st.bus_busy, (unsigned long)st.chan_busy, (unsigned long)st.slot_stall);
        static const char *ln[] = { "read", "write", "flush" };
        for (int k = 0; k < 3; k++) {
            uint64_t n;
            uint64_t p50 = lat_pct(st.lat[k], 50, &n);
            uint64_t p99 = lat_pct(st.lat[k], 99, &n);
            uint64_t p999 = lat_pct(st.lat[k], 99.9, &n);
            fprintf(f, "%s_lat_n=%lu\n%s_lat_p50_us=%lu\n%s_lat_p99_us=%lu\n%s_lat_p999_us=%lu\n",
                    ln[k], (unsigned long)n, ln[k], (unsigned long)p50, ln[k], (unsigned long)p99,
                    ln[k], (unsigned long)p999);
        }
        return;
    }
        fprintf(f,
                "emmcsim  elapsed %.3f s since reset  dies %d x planes %d  "
                "page %u KB  time_scale %g\n",
            el / 1e9, cfg.dies, cfg.planes, cfg.page / 1024, cfg.time_scale);
    fprintf(f, "host   read %lu req %.1f MB   write %lu req %.1f MB   flush %lu   fua %lu\n",
            (unsigned long)st.host_reads, st.bytes_read / 1e6, (unsigned long)st.host_writes,
            st.bytes_written / 1e6, (unsigned long)st.host_flushes, (unsigned long)st.host_fua);
    fprintf(f,
            "nand   page reads %lu (buffer hits %lu, retries %lu)   "
            "programs %lu ops, %lu pages = %.1f MB   padding %.2f MB   erases %lu\n",
            (unsigned long)st.page_reads, (unsigned long)st.buffer_hits, (unsigned long)st.retries,
            (unsigned long)st.prog_ops, (unsigned long)st.pages_programmed, nand_bytes / 1e6,
            st.pad_bytes / 1e6, (unsigned long)st.erases);
    if (st.bytes_written)
        fprintf(f, "       NAND bytes / host bytes written = %.2f\n",
                (double)nand_bytes / st.bytes_written);
    for (int d = 0; d < cfg.dies; d++) {
        uint64_t *b = st.die_busy[d], all = b[OP_READ] + b[OP_PROG] + b[OP_ERASE] + st.die_redo[d];
        fprintf(f,
                "die%-2d  busy %8.1f ms %5.1f%%   read %5.1f%%  "
                "program %5.1f%%  erase %5.1f%%  redo %4.1f%% "
                "(%7.3f ms)  suspend %lu\n",
                d, all / 1e6, pct(all, el), pct(b[OP_READ], el),
                pct(b[OP_PROG], el), pct(b[OP_ERASE], el),
                pct(st.die_redo[d], el), st.die_redo[d] / 1e6, (unsigned long)st.die_susp[d]);
    }
    fprintf(f, "bus    busy %8.1f ms %5.1f%%   nand channel busy %5.1f%%\n",
            st.bus_busy / 1e6, pct(st.bus_busy, el), pct(st.chan_busy, el));
    if (st.slot_stall)
        fprintf(f,
                "SRAM   writes waited for a free slot: %.3f s in total "
                "(sum over requests)\n",
                st.slot_stall / 1e9);
    static const char *ln[] = { "read ", "write", "flush" };
    for (int k = 0; k < 3; k++) {
        uint64_t n;
        uint64_t p50 = lat_pct(st.lat[k], 50, &n);
        uint64_t p99 = lat_pct(st.lat[k], 99, &n);
        uint64_t p999 = lat_pct(st.lat[k], 99.9, &n);
        if (n) {
            fprintf(f,
                    "%s  latency inside the device: p50 %lu us  p99 %lu us  "
                    "p99.9 %lu us  (%lu requests)\n",
                    ln[k], (unsigned long)p50, (unsigned long)p99,
                    (unsigned long)p999, (unsigned long)n);
        }
    }
}

void model_trace(FILE *f, int clear)
{
    prune(now_ns());
    static const char *name[] = { "read", "program", "erase" };
    fprintf(f, "kind,die,start_us,end_us,aux\n");
    for (uint64_t i = 0; i < trace_n; i++) {
        struct ev *e = &trace[i];
        const char *k = e->kind < OP_NKIND ? name[e->kind] :
                        e->kind == EV_HREAD ? "host_read" :
                        e->kind == EV_HWRITE ? "host_write" : "host_flush";
        fprintf(f, "%s,%d,%.1f,%.1f,%u\n", k, e->die,
                (int64_t)(e->t0 - trace_t0) / 1e3, (int64_t)(e->t1 - trace_t0) / 1e3, e->aux);
    }
    if (clear) {
        trace_n = 0;
        trace_t0 = now_ns();
    }
}
