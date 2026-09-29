#include "log_stats.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool valid_status(unsigned status) {
    return status == 200 || status == 201 || status == 204 || status == 400 || status == 404 ||
           status == 429 || status == 500 || status == 503;
}

bool load_log(const char *path, struct log_data *data) {
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return false;
    }

    char line[128];
    if (!fgets(line, sizeof line, file) ||
        strcmp(line, "timestamp_ms,service_id,status,duration_us\n")) {
        fprintf(stderr, "Unexpected CSV header in %s\n", path);
        fclose(file);
        return false;
    }

    size_t capacity = 262144;
    data->records = malloc(capacity * sizeof *data->records);
    if (!data->records) {
        perror("malloc records");
        fclose(file);
        return false;
    }

    uint64_t previous_timestamp = 0;
    while (fgets(line, sizeof line, file)) {
        unsigned long long timestamp;
        unsigned service_id, status, duration_us;
        char ending;

        if (sscanf(line, "%llu,%u,%u,%u%c", &timestamp, &service_id, &status, &duration_us,
                   &ending) != 5 ||
            ending != '\n' || timestamp == 0 || timestamp < previous_timestamp ||
            service_id >= SERVICE_COUNT || !valid_status(status) || duration_us == 0 ||
            duration_us > 10000000) {
            fprintf(stderr, "Invalid CSV row %zu in %s\n", data->count + 2, path);
            free(data->records);
            data->records = NULL;
            fclose(file);
            return false;
        }

        previous_timestamp = (uint64_t)timestamp;
        if (data->count == capacity) {
            if (capacity > SIZE_MAX / 2 / sizeof *data->records) {
                fprintf(stderr, "Too many CSV rows\n");
                free(data->records);
                data->records = NULL;
                fclose(file);
                return false;
            }

            capacity *= 2;
            struct log_record *larger = realloc(data->records, capacity * sizeof *data->records);
            if (!larger) {
                perror("realloc records");
                free(data->records);
                data->records = NULL;
                fclose(file);
                return false;
            }

            data->records = larger;
        }

        data->records[data->count++] = (struct log_record){
            .service_id = (uint16_t)service_id,
            .status = (uint16_t)status,
            .duration_us = duration_us,
        };
    }

    bool good = !ferror(file) && data->count > 0;
    fclose(file);
    if (!good) {
        fprintf(stderr, "Empty or unreadable CSV: %s\n", path);
        free(data->records);
        data->records = NULL;
    }

    return good;
}

static inline void add_record(struct service_stats *stats, const struct log_record *record) {
    ++stats->requests;
    stats->errors += record->status >= 400;
    stats->duration_us += record->duration_us;

    if (stats->requests == 1 || record->duration_us < stats->min_us) {
        stats->min_us = record->duration_us;
    }

    if (record->duration_us > stats->max_us) {
        stats->max_us = record->duration_us;
    }
}

/* KEY_RESCAN: each service searches every record again. Real aggregation, O(S x N). */
__attribute__((noinline)) void aggregate_rescan(const struct log_data *data,
                                                struct service_stats result[SERVICE_COUNT]) {
    memset(result, 0, SERVICE_COUNT * sizeof *result);

    for (size_t service = 0; service < SERVICE_COUNT; ++service) { /* KEY_NESTED_LOOP */
        for (size_t row = 0; row < data->count; ++row) {
            if (data->records[row].service_id == service) {
                add_record(&result[service], &data->records[row]);
            }
        }
    }
}

/* KEY_ONEPASS: one traversal updates the requested service's accumulator. */
__attribute__((noinline)) void aggregate_onepass(const struct log_data *data,
                                                 struct service_stats result[SERVICE_COUNT]) {
    memset(result, 0, SERVICE_COUNT * sizeof *result);

    for (size_t row = 0; row < data->count; ++row) { /* KEY_SINGLE_LOOP */
        const struct log_record *record = &data->records[row];
        add_record(&result[record->service_id], record);
    }
}

struct totals summarize(const struct service_stats stats[SERVICE_COUNT]) {
    struct totals result = {0};

    for (size_t service = 0; service < SERVICE_COUNT; ++service) {
        const struct service_stats *item = &stats[service];
        result.requests += item->requests;
        result.errors += item->errors;
        result.duration_us += item->duration_us;
        result.signature += (service + 1) * (item->requests + item->errors + item->duration_us +
                                             item->min_us + item->max_us);
    }

    return result;
}
