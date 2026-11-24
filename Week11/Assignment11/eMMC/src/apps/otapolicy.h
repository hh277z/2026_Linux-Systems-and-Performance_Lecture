/*
 * otapolicy.h: when an OTA writer may send the next chunk (Assignment 11, Level 3).
 *
 * otawriter -m policy writes the update with O_DIRECT in chunks. Before each
 * chunk it calls op_wait(). The policy may sleep there to
 *   - keep the device write rate at or below rate_mbps (0 = no limit), and
 *   - stay quiet while the UI is reading: after the device has completed a
 *     read, send no write for quiet_ms.
 * The UI's reads are visible in /sys/block/<dev>/stat (field 1: reads completed).
 * The OTA writer itself only writes, so any new read there belongs to someone else.
 */
#ifndef OTAPOLICY_H
#define OTAPOLICY_H
#include <stdint.h>

struct op_params {
    const char *stat_path;     /* e.g. /sys/block/nbd0/stat */
    double rate_mbps;          /* 0 = no rate limit */
    uint32_t quiet_ms;         /* 0 = do not watch the UI */
};

struct ota_policy;

struct ota_policy *op_open(const struct op_params *p);
void op_wait(struct ota_policy *op, uint64_t chunk_bytes);   /* may sleep before a chunk */
uint64_t op_paused_ns(const struct ota_policy *op);          /* time spent waiting for the UI */
void op_close(struct ota_policy *op);

#endif
