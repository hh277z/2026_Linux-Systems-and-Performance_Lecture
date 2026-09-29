#ifndef WEEK3_LOG_STATS_H
#define WEEK3_LOG_STATS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SERVICE_COUNT 1024

struct log_record {
    uint16_t service_id;
    uint16_t status;
    uint32_t duration_us;
};

struct log_data {
    struct log_record *records;
    size_t count;
};

struct service_stats {
    uint64_t requests;
    uint64_t errors;
    uint64_t duration_us;
    uint32_t min_us;
    uint32_t max_us;
};

struct totals {
    uint64_t requests;
    uint64_t errors;
    uint64_t duration_us;
    uint64_t signature;
};

bool load_log(const char *path, struct log_data *data);
void aggregate_rescan(const struct log_data *data, struct service_stats result[SERVICE_COUNT]);
void aggregate_onepass(const struct log_data *data, struct service_stats result[SERVICE_COUNT]);
struct totals summarize(const struct service_stats stats[SERVICE_COUNT]);

#endif
