/*
 * bq.c - bounded blocking queue (Level 1).
 *
 * The helpers and bq_init/bq_destroy are provided. Implement the four
 * functions marked TODO(L1-n). Read the contract of each one in bq.h.
 */
#include <errno.h>
#include <stdlib.h>
#include <time.h>

#include "bq.h"

/* ---- provided helpers ------------------------------------------------ */

/* Absolute CLOCK_MONOTONIC time `ms` milliseconds from now. */
static inline struct timespec deadline_after_ms(int ms)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec  += ms / 1000;
    t.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec  += 1;
        t.tv_nsec -= 1000000000L;
    }
    return t;
}

/* Wake one waiter, or all of them with BQ_BROADCAST. */
static inline void wake(bq_t *q, pthread_cond_t *cv)
{
    if (q->flags & BQ_BROADCAST)
        pthread_cond_broadcast(cv);
    else
        pthread_cond_signal(cv);
}

/* Ring buffer operations. Call with q->lock held. */
static inline void push_item(bq_t *q, bq_item_t item)
{
    q->buf[q->tail] = item;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;
}

static inline bq_item_t pop_item(bq_t *q)
{
    bq_item_t item = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return item;
}

int bq_init(bq_t *q, int capacity, int flags)
{
    if (capacity < 1)
        return -1;
    q->buf = calloc((size_t)capacity, sizeof q->buf[0]);
    if (!q->buf)
        return -1;
    q->cap = capacity;
    q->head = q->tail = q->count = 0;
    q->closed = 0;
    q->flags = flags;
    q->n_put_waits = q->n_get_waits = 0;

    pthread_mutex_init(&q->lock, NULL);

    /* not_empty is used by bq_timed_get, so it measures time on CLOCK_MONOTONIC. */
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&q->not_empty, &ca);
    pthread_cond_init(&q->not_full, &ca);
    pthread_condattr_destroy(&ca);
    return 0;
}

void bq_destroy(bq_t *q)
{
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
    pthread_mutex_destroy(&q->lock);
    free(q->buf);
    q->buf = NULL;
}

/* ---- Level 1 --------------------------------------------------------- */

/*
 * TODO(L1-1)
 *  - Wait while the queue is full and not closed. Use BQ_WHILE, not while,
 *    and count every sleep in q->n_put_waits.
 *  - If the queue is closed (before or during the wait), return BQ_CLOSED
 *    without enqueuing.
 *  - Otherwise push the item, wake a consumer with wake(), and return BQ_OK.
 */
int bq_put(bq_t *q, bq_item_t item)
{
    (void)q;
    (void)item;
    return BQ_CLOSED;               /* replace this stub */
}

/*
 * TODO(L1-2)
 *  - Wait while the queue is empty and not closed (BQ_WHILE, count every
 *    sleep in q->n_get_waits).
 *  - If an item is there, pop it into *out, wake a producer, return BQ_OK.
 *    Remaining items are returned even after close.
 *  - If the queue is empty and closed, return BQ_CLOSED.
 */
int bq_get(bq_t *q, bq_item_t *out)
{
    (void)q;
    (void)out;
    return BQ_CLOSED;               /* replace this stub */
}

/*
 * TODO(L1-3)
 *  - Compute the deadline once, before taking the lock (deadline_after_ms).
 *  - Wait with BQ_WHILE and pthread_cond_timedwait on not_empty, which uses
 *    CLOCK_MONOTONIC, and count every sleep in q->n_get_waits. A wake-up
 *    without an item must not move the deadline.
 *  - timeout_ms == 0 must not block at all.
 *  - Decide in this order: item available -> pop it, wake a producer, BQ_OK.
 *    Else closed -> BQ_CLOSED. Else -> BQ_TIMEOUT.
 */
int bq_timed_get(bq_t *q, bq_item_t *out, int timeout_ms)
{
    (void)q;
    (void)out;
    (void)timeout_ms;
    return BQ_TIMEOUT;              /* replace this stub */
}

/*
 * TODO(L1-4)
 *  - Mark the queue closed under the lock.
 *  - Wake every waiting producer and consumer, whatever BQ_BROADCAST says.
 *  - Calling it twice must be harmless.
 */
void bq_close(bq_t *q)
{
    (void)q;                        /* replace this stub */
}
