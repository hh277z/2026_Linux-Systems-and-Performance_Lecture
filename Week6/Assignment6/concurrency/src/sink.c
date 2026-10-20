/*
 * sink.c - case 1: audio sinks (Level 3).
 *
 * Every sink has its own mutex that protects its session count. Moving a
 * session changes two sinks, so sink_move holds both sink locks. sink_total
 * holds every sink lock at once, so it counts all sessions at one instant.
 *
 * Run tools/l3_run.sh 1 to see why this file can deadlock, then fix it
 * inside TODO(L3-1). The rest of the file is provided.
 */
#include "devsvc.h"

struct sink sinks[NSINKS];

void sink_init(void)
{
    for (int i = 0; i < NSINKS; i++) {
        pthread_mutex_init(&sinks[i].lock, NULL);
        sinks[i].id = i;
        sinks[i].sessions = SESSIONS_PER_SINK;
    }
}

/* Takes the sink locks from sinks[0] up to sinks[NSINKS - 1]. */
long sink_total(void)
{
    long t = 0;
    for (int i = 0; i < NSINKS; i++)
        pthread_mutex_lock(&sinks[i].lock);
    for (int i = 0; i < NSINKS; i++)
        t += sinks[i].sessions;
    for (int i = NSINKS - 1; i >= 0; i--)
        pthread_mutex_unlock(&sinks[i].lock);
    return t;
}

/* ==== TODO(L3-1) begin ==================================================
 * Keep one pthread_mutex_t per sink. Change the order in which sink_move
 * takes the two locks. Keep svc_work inside both sink locks.
 */

int sink_move(int from, int to)
{
    struct sink *a = &sinks[from], *b = &sinks[to];
    int moved = 0;

    pthread_mutex_lock(&a->lock);
    pthread_mutex_lock(&b->lock);
    if (a->sessions > 0) {
        a->sessions--;
        b->sessions++;
        b->moved_in++;
        moved = 1;
    }
    svc_work(WORK_SINK);
    pthread_mutex_unlock(&b->lock);
    pthread_mutex_unlock(&a->lock);
    return moved;
}

/* ==== TODO(L3-1) end ==================================================== */
