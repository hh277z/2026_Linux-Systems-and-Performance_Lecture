#include "lab_common.h"

static void *entries[512];

__attribute__((noinline)) static void cache_store_add(size_t index)
{
    entries[index] = malloc(16 * 1024);
    if (!entries[index]) { perror("malloc"); exit(1); }
    touch_pages(entries[index], 16 * 1024);
}

__attribute__((noinline)) static void cache_store_expire(int fixed)
{
    for (size_t i = 0; i < 512; i++) {
        /* The buggy path loses ownership of expired cache entries. */
        if (fixed) free(entries[i]);
        entries[i] = NULL;
    }
}

static void process_four_batches(int fixed)
{
    for (int batch = 0; batch < 4; batch++) {
        for (size_t i = 0; i < 512; i++) cache_store_add(i);
        cache_store_expire(fixed);
    }
}

int main(int argc, char **argv)
{
    if ((argc != 3 && argc != 4) || (strcmp(argv[1], "buggy") && strcmp(argv[1], "fixed")) ||
        (argc == 4 && strcmp(argv[3], "--auto") && strcmp(argv[3], "--demo"))) {
        fputs("Usage: lab10_a buggy|fixed CGROUP [--auto|--demo]\n", stderr); return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    join_group(argv[2]);
    int automatic = argc == 4;
    int fixed = !strcmp(argv[1], "fixed");
    printf("MODE=%s PAGE_SIZE=%ld batch_payload=8MiB\n", argv[1], sysconf(_SC_PAGESIZE));
    if (argc == 4 && !strcmp(argv[3], "--demo")) {
        puts("DEMO: 12 batches, one batch per second; no ENTER is needed.");
        sleep(3);
        for (int batch = 1; batch <= 12; batch++) {
            for (size_t i = 0; i < 512; i++) cache_store_add(i);
            cache_store_expire(fixed);
            printf("batch=%2d/12  cumulative payload processed=%3d MiB  expired this batch=512 entries\n",
                   batch, batch * 8);
            sleep(1);
        }
        puts("DEMO: workload finished; holding process for 3 seconds before exit.");
        sleep(3);
        return 0;
    }
    if (!stage(1, "BASELINE", "process four batches and expire entries", automatic)) return 2;
    process_four_batches(fixed);
    if (!stage(2, "AFTER_4_BATCHES", "process four more batches", automatic)) return 2;
    process_four_batches(fixed);
    if (!stage(3, "AFTER_8_BATCHES", "process final four batches", automatic)) return 2;
    process_four_batches(fixed);
    if (!stage(4, "AFTER_12_BATCHES", "exit", automatic)) return 2;
    return 0;
}
