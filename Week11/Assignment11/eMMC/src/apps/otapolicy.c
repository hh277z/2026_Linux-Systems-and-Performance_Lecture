/*
 * otapolicy.c: when the OTA writer may send its next chunk (Assignment 11, Level 3).
 *
 * Read otapolicy.h first: it is the specification.
 *
 * This skeleton builds and runs: op_wait never waits, so the OTA writes as fast
 * as the device takes it. Replace the TODO parts with
 *   - a rate limit: the average write rate stays at or below rate_mbps, and
 *   - a quiet rule: after the device completes a read that the writer did not
 *     issue, send no chunk for quiet_ms (lecture p.45: big writes when the TV is idle).
 * Keep the function names and signatures.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "otapolicy.h"

struct ota_policy {
    struct op_params p;
    uint64_t paused_ns;
    /* TODO(L3-1): what the rate limit and the quiet rule need to remember */
};

/* Reads completed on the device so far: field 1 of /sys/block/<dev>/stat. */
static uint64_t reads_completed(const char *path)
{
    unsigned long long r = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%llu", &r) != 1) {
            r = 0;
        }
        fclose(f);
    }
    return r;
}

struct ota_policy *op_open(const struct op_params *p)
{
    struct ota_policy *op = calloc(1, sizeof *op);
    if (!op) {
        return NULL;
    }
    op->p = *p;
    (void)reads_completed;
    /* TODO(L3-1): start the clocks and counters */
    return op;
}

void op_wait(struct ota_policy *op, uint64_t chunk_bytes)
{
    (void)op;
    (void)chunk_bytes;
    /* TODO(L3-2): rate limit. Sleep until the bytes sent so far fit rate_mbps.
     * TODO(L3-3): quiet rule. Poll reads_completed() about every millisecond; wait until
     *             no new read has completed for quiet_ms. Add the waiting time to paused_ns. */
}

uint64_t op_paused_ns(const struct ota_policy *op)
{
    return op->paused_ns;
}

void op_close(struct ota_policy *op)
{
    free(op);
}
