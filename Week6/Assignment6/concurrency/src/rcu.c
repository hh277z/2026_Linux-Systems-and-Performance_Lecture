/*
 * rcu.c - user-space QSBR RCU (provided, do not modify). See rcu.h.
 *
 * gp_seq counts grace periods. A registered thread stores the gp_seq it has
 * seen in its slot every time it passes a quiescent state. synchronize_rcu
 * advances gp_seq to a new target and waits until every other slot has seen
 * the target. A slot of 0 means the thread is offline (not registered, or
 * inside synchronize_rcu) and is not waited for.
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rcu.h"

#define MAX_RCU_THREADS 64
#define OFFLINE 0ul

static struct { atomic_ulong seq; } __attribute__((aligned(64))) gp_seq = { 1 };

static struct {
    atomic_ulong seen;         /* last gp_seq seen in a quiescent state, 0: offline */
    atomic_int   used;
} __attribute__((aligned(64))) slot[MAX_RCU_THREADS];

static __thread int my_slot = -1;

static void report_qs(int i)
{
    unsigned long seq = atomic_load_explicit(&gp_seq.seq, memory_order_acquire);
    atomic_store_explicit(&slot[i].seen, seq, memory_order_release);
}

/* An offline thread comes online. A writer that checked this slot while it
 * was 0 skipped it. This is the store-buffer pattern of Lab 6, so both sides
 * use seq_cst on gp_seq and seen: either the writer sees this slot online and
 * waits for it, or this thread reads the writer's new gp_seq and with it sees
 * everything the writer published before. */
static void go_online(int i)
{
    unsigned long seq = atomic_load_explicit(&gp_seq.seq, memory_order_relaxed);
    atomic_exchange_explicit(&slot[i].seen, seq, memory_order_seq_cst);
    unsigned long now = atomic_load_explicit(&gp_seq.seq, memory_order_seq_cst);
    if (now != seq)
        atomic_store_explicit(&slot[i].seen, now, memory_order_release);
}

void rcu_register_thread(void)
{
    for (int i = 0; i < MAX_RCU_THREADS; i++) {
        int unused = 0;
        if (atomic_compare_exchange_strong(&slot[i].used, &unused, 1)) {
            my_slot = i;
            go_online(i);
            return;
        }
    }
    fprintf(stderr, "rcu: more than %d threads\n", MAX_RCU_THREADS);
    abort();
}

void rcu_unregister_thread(void)
{
    if (my_slot < 0)
        return;
    atomic_store_explicit(&slot[my_slot].seen, OFFLINE, memory_order_release);
    atomic_store(&slot[my_slot].used, 0);
    my_slot = -1;
}

void rcu_quiescent_state(void)
{
    if (my_slot >= 0)
        report_qs(my_slot);
}

void synchronize_rcu(void)
{
    unsigned long target = atomic_fetch_add_explicit(&gp_seq.seq, 1, memory_order_seq_cst) + 1;

    /* The caller is outside any read-side section, so it is offline while it waits. */
    if (my_slot >= 0)
        atomic_store_explicit(&slot[my_slot].seen, OFFLINE, memory_order_release);

    for (int i = 0; i < MAX_RCU_THREADS; i++) {
        if (i == my_slot || !atomic_load(&slot[i].used))
            continue;
        for (;;) {
            unsigned long s = atomic_load_explicit(&slot[i].seen, memory_order_seq_cst);  /* see go_online */
            if (s == OFFLINE || (long)(s - target) >= 0)
                break;
            usleep(50);
        }
    }

    if (my_slot >= 0)
        go_online(my_slot);
}
