/*
 * bq.h - bounded blocking queue (Level 1).
 *
 * Do not change this interface. bqtest and albumsvc are written against it,
 * and lib/libbq_ref.a implements exactly the same functions.
 */
#ifndef BQ_H
#define BQ_H

#include <pthread.h>
#include <stdint.h>

/* Return values of bq_put, bq_get and bq_timed_get. */
enum bq_status {
    BQ_OK      = 0,   /* an item was put or taken                         */
    BQ_TIMEOUT = 1,   /* bq_timed_get only: no item before the deadline   */
    BQ_CLOSED  = 2,   /* the queue is closed (and, for get, empty)        */
};

/* Flags for bq_init. */
#define BQ_BROADCAST 0x1  /* wake waiters with broadcast instead of signal (experiment knob) */

/*
 * One queue entry. The queue copies the struct and never looks inside it.
 *   bqtest:   id = (producer << 32) | sequence, kind and ptr unused
 *   albumsvc: kind tells what ptr points to; ownership of *ptr moves with the item
 */
typedef struct {
    uint64_t id;
    int      kind;
    void    *ptr;
} bq_item_t;

typedef struct {
    pthread_mutex_t lock;       /* protects every field below                 */
    pthread_cond_t  not_empty;  /* consumers wait here (CLOCK_MONOTONIC)      */
    pthread_cond_t  not_full;   /* producers wait here                        */

    bq_item_t *buf;             /* ring buffer of `cap` entries               */
    int cap;
    int head;                   /* next slot to take                          */
    int tail;                   /* next slot to fill                          */
    int count;                  /* number of items in the queue               */
    int closed;                 /* set once by bq_close, never cleared        */
    int flags;                  /* BQ_BROADCAST or 0                          */

    /* Statistics, updated under `lock`. bqtest prints them. */
    uint64_t n_put_waits;       /* times a producer went to sleep on not_full */
    uint64_t n_get_waits;       /* times a consumer went to sleep on not_empty */
} bq_t;

/*
 * The wait loop keyword. Write every condition wait loop with BQ_WHILE
 * instead of while:
 *     BQ_WHILE (condition) {
 *         count this sleep in n_put_waits or n_get_waits;
 *         pthread_cond_wait(...);
 *     }
 * A normal build turns it into `while`. `make bugif` builds bqtest with
 * -DBUG_IF, which turns it into `if` for explanation 1.
 */
#ifdef BUG_IF
#define BQ_WHILE if
#else
#define BQ_WHILE while
#endif

/* Returns 0, or -1 if capacity < 1 or memory allocation fails. */
int  bq_init(bq_t *q, int capacity, int flags);

/* Call only after every thread that used q has returned from all bq_* calls
 * (for example after pthread_join). Items still in the queue are dropped. */
void bq_destroy(bq_t *q);

/* Blocks while the queue is full. Returns BQ_OK, or BQ_CLOSED without
 * enqueuing if the queue is closed before or while waiting. */
int  bq_put(bq_t *q, bq_item_t item);

/* Blocks while the queue is empty. Returns BQ_OK with *out filled, or
 * BQ_CLOSED once the queue is closed and empty (remaining items drain first). */
int  bq_get(bq_t *q, bq_item_t *out);

/* Like bq_get, but waits at most timeout_ms (>= 0, relative, CLOCK_MONOTONIC).
 * The deadline is computed once on entry. timeout_ms == 0 never blocks.
 * Priority when several hold: item available -> BQ_OK, else closed -> BQ_CLOSED,
 * else deadline passed -> BQ_TIMEOUT. */
int  bq_timed_get(bq_t *q, bq_item_t *out, int timeout_ms);

/* Marks the queue closed and wakes every waiting producer and consumer,
 * whatever BQ_BROADCAST says. Calling it again has no further effect. */
void bq_close(bq_t *q);

#endif /* BQ_H */
