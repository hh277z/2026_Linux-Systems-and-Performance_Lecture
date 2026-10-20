/*
 * bus.c - case 2: event bus (Level 3).
 *
 * subs[0..nsubs-1] is the list of subscribers, protected by bus_lock.
 * bus_publish calls the callback of every subscriber. The contract is in
 * devsvc.h: a callback may call bus_subscribe or bus_unsubscribe, and the
 * driver's one-shot subscribers unsubscribe themselves from their callback.
 *
 * Run tools/l3_run.sh 2 to see why this file can deadlock, then fix it
 * inside TODO(L3-2). The rest of the file is provided.
 */
#include <string.h>

#include "devsvc.h"

static struct sub *subs[MAX_SUBS];
static int nsubs;

/* Position of s in subs[], or -1. The caller holds bus_lock. */
static int find(struct sub *s)
{
    for (int i = 0; i < nsubs; i++)
        if (subs[i] == s)
            return i;
    return -1;
}

/* ==== TODO(L3-2) begin ==================================================
 * Fix the deadlock without a recursive mutex: a callback must be able to
 * call back into the bus.
 */

pthread_mutex_t bus_lock;

void bus_init(void)
{
    pthread_mutex_init(&bus_lock, NULL);
    nsubs = 0;
}

int bus_subscribe(struct sub *s)
{
    int added = 0;
    pthread_mutex_lock(&bus_lock);
    if (find(s) < 0 && nsubs < MAX_SUBS) {
        subs[nsubs++] = s;
        added = 1;
    }
    pthread_mutex_unlock(&bus_lock);
    return added;
}

int bus_unsubscribe(struct sub *s)
{
    pthread_mutex_lock(&bus_lock);
    int i = find(s);
    if (i >= 0)
        subs[i] = subs[--nsubs];        /* the last one takes its place */
    pthread_mutex_unlock(&bus_lock);
    return i >= 0;
}

void bus_publish(int ev)
{
    pthread_mutex_lock(&bus_lock);
    for (int i = 0; i < nsubs; i++)
        subs[i]->cb(subs[i], ev);
    pthread_mutex_unlock(&bus_lock);
}

/* ==== TODO(L3-2) end ==================================================== */
