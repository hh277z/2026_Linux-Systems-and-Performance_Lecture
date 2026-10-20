/*
 * devsvc.h - Level 3: a small device service with four deadlock-prone parts
 * (provided, do not modify).
 *
 * Each case is one file, and bin/devsvc -c <case> stresses only that file.
 *   case 1  src/sink.c     audio sinks: move a session from one sink to another
 *   case 2  src/bus.c      event bus: callbacks that call back into the bus
 *   case 3  src/catalog.c  app catalog: apps and permissions looked up from both sides
 *   case 4  src/config.c   picture config that every frame reads
 * Change only the regions marked TODO(L3-n). src/devsvc.c (the driver),
 * src/rcu.h and src/rcu.c are provided as well.
 *
 * Every function below may be called by several threads at the same time.
 *
 * svc_work(WORK_*) stands for the real work of a request. Do not remove a
 * call or move it out of the section it is in now.
 */
#ifndef DEVSVC_H
#define DEVSVC_H

#include <pthread.h>
#include <stdatomic.h>

/* Busy work that stands for real work: roughly `n` loop iterations. */
void svc_work(int n);

/* Fixed amounts of work. They are part of the test, like bqtest's checks. */
#define WORK_SINK     3000     /* sink_move, inside the sink locks                  */
#define WORK_CB       300      /* every bus callback                                */
#define WORK_LOOKUP   3000     /* every catalog lookup or update, inside its locks  */
#define WORK_FRAME    3000     /* output_render, drawing the frame                  */
#define WORK_OUTSIDE  200      /* every request, outside all locks                  */

/* ---- case 1: sinks (src/sink.c) --------------------------------------- */

#define NSINKS            16
#define SESSIONS_PER_SINK 100

/* 64 bytes each: lockcheck prints sinks+0x40 for sinks[1].lock. */
struct sink {
    pthread_mutex_t lock;      /* protects sessions and moved_in */
    int  id;
    int  sessions;
    long moved_in;
} __attribute__((aligned(64)));

extern struct sink sinks[NSINKS];

void sink_init(void);
/* Move one session from sink `from` to sink `to` (from != to). Returns 1 if a
 * session moved, 0 if `from` had none. */
int  sink_move(int from, int to);
/* Sessions in all sinks at one instant. Always NSINKS * SESSIONS_PER_SINK. */
long sink_total(void);

/* ---- case 2: event bus (src/bus.c) ------------------------------------ */

#define MAX_SUBS 64

struct sub {
    void (*cb)(struct sub *s, int ev);   /* set by the driver                */
    int id;
    atomic_long calls;                   /* the driver's own bookkeeping     */
};

void bus_init(void);
/* 1 if s was added, 0 if it was already subscribed (or the bus is full). */
int  bus_subscribe(struct sub *s);
/* 1 if s was removed, 0 if it was not subscribed. */
int  bus_unsubscribe(struct sub *s);
/*
 * Call s->cb(s, ev) once for every subscriber s.
 *  - A subscriber that stays subscribed for the whole call is called exactly once.
 *  - One that is added or removed during the call may be called or not.
 *  - A callback may call bus_subscribe and bus_unsubscribe, also for itself.
 *  - Callbacks may run at the same time on different threads. The driver's
 *    callbacks are thread-safe, and the driver never frees a struct sub.
 */
void bus_publish(int ev);

/* ---- case 3: app catalog (src/catalog.c) ------------------------------- */

#define NAPPS  8
#define NPERMS 8

/* The rule the catalog keeps: an app never holds a disabled permission. */
void catalog_init(void);
/* 1 if `app` holds `perm` and `perm` is enabled, else 0. */
int  catalog_can_launch(int app, int perm);
/* Number of apps that hold `perm`, and in *enabled whether it is enabled. */
int  catalog_perm_users(int perm, int *enabled);
/* Give `perm` to `app` if `perm` is enabled. 1 if the app holds it now. */
int  catalog_grant(int app, int perm);
/* Take `perm` away from `app`. */
void catalog_revoke(int app, int perm);
/* Enable or disable `perm`. Disabling is refused while an app holds it.
 * Returns 1 if the permission is in the requested state now. */
int  catalog_set_perm(int perm, int enabled);

/* pthread_rwlock_init with the kind the driver was asked for: the glibc
 * default, or writer-preferring when DEVSVC_RWLOCK=writer is set. */
void svc_rwlock_init(pthread_rwlock_t *l);

/* ---- case 4: picture config (src/config.c) ----------------------------- */

#define NOUTPUTS   16
#define CFG_FIELDS 8

/* A consistent config has v[i] == cfg_value(version) for every i. */
struct config {
    unsigned version;
    int v[CFG_FIELDS];
};
#define cfg_value(version) ((int)((version) % 1000u) + 1)

struct output {
    pthread_mutex_t lock;      /* protects the fields below */
    int      id;
    long     frames;
    unsigned shown;            /* config version of the last frame          */
    unsigned applied;          /* config version applied by config_update   */
} __attribute__((aligned(64)));

extern struct output outputs[NOUTPUTS];

void config_init(void);
/* Render one frame on output `out` with the current config. Returns 1 if the
 * config it read was consistent, 0 if not. */
int  output_render(int out);
/* Publish config version + 1 and apply it to every output. */
void config_update(void);
/* Version of the current config. The driver calls it only after every
 * worker has stopped. */
unsigned config_version(void);

#endif
