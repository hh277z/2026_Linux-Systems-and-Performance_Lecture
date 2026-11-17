#include "lab_common.h"
#include <sys/mman.h>

static void *allocate(size_t size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(1); }
    touch_pages(p, size);
    return p;
}

int main(int argc, char **argv)
{
    if ((argc != 2 && argc != 3) ||
        (argc == 3 && strcmp(argv[2], "--auto") && strcmp(argv[2], "--demo"))) {
        fputs("Usage: lab10_b CGROUP [--auto|--demo]\n", stderr); return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    join_group(argv[1]);
    int automatic = argc == 3, result = 2;
    void *base = allocate(48 * MIB), *temporary = MAP_FAILED;
    if (argc == 3 && !strcmp(argv[2], "--demo")) {
        puts("DEMO: 48MiB resident buffer; each playback uses an extra 80MiB.");
        for (int cycle = 1; cycle <= 2; cycle++) {
            printf("cycle=%d IDLE: holding 48MiB for 3 seconds\n", cycle);
            sleep(3);
            printf("cycle=%d PLAYBACK: allocating and touching 80MiB\n", cycle);
            temporary = allocate(80 * MIB);
            printf("cycle=%d PEAK: holding both buffers for 3 seconds\n", cycle);
            sleep(3);
            munmap(temporary, 80 * MIB); temporary = MAP_FAILED;
            printf("cycle=%d RETURNED: temporary buffer released\n", cycle);
            sleep(3);
        }
        result = 0;
        goto cleanup;
    }
    if (!stage(1, "IDLE", "allocate and touch an extra 80MiB", automatic)) goto cleanup;
    temporary = allocate(80 * MIB);
    if (!stage(2, "PEAK", "release temporary buffer", automatic)) goto cleanup;
    munmap(temporary, 80 * MIB); temporary = MAP_FAILED;
    if (!stage(3, "RETURNED", "repeat the same peak", automatic)) goto cleanup;
    temporary = allocate(80 * MIB);
    if (!stage(4, "SECOND_PEAK", "release temporary buffer again", automatic)) goto cleanup;
    munmap(temporary, 80 * MIB); temporary = MAP_FAILED;
    if (!stage(5, "RETURNED_AGAIN", "exit", automatic)) goto cleanup;
    result = 0;
cleanup:
    if (temporary != MAP_FAILED) munmap(temporary, 80 * MIB);
    munmap(base, 48 * MIB);
    return result;
}
