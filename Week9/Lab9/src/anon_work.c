#include "common.h"
#include <string.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[3], "hold") && strcmp(argv[3], "touch"))) {
        fprintf(stderr, "usage: anon_work MIB SECONDS hold|touch\n");
        return 2;
    }

    size_t bytes = (size_t)number(argv[1], 8, 512) * 1024 * 1024;
    long seconds = number(argv[2], 1, 30);
    long page = sysconf(_SC_PAGESIZE);

    if (page <= 0) {
        fail("page size");
    }

    unsigned char *data = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (data == MAP_FAILED) {
        fail("mmap");
    }

    /* KEY: reserve first; write each page so allocation consumes physical memory. */
    for (size_t offset = 0; offset < bytes; offset += (size_t)page) {
        data[offset] = 1;
    }

    ready();
    double start = now();
    volatile unsigned long sum = 0;

    while (now() - start < seconds) {
        if (!strcmp(argv[3], "touch")) {
            /* KEY: revisiting anonymous pages can read previously swapped pages. */
            for (size_t offset = 0; offset < bytes; offset += (size_t)page) {
                sum += data[offset];
            }

        } else {
            struct timespec delay = { .tv_nsec = 100000000 };
            nanosleep(&delay, NULL);
        }

    }

    printf("checksum=%lu seconds=%.6f\n", sum, now() - start);

    /* KEY: releasing the mapping returns the workload's anonymous pages. */
    if (munmap(data, bytes) != 0) {
        fail("munmap");
    }

    return 0;
}
