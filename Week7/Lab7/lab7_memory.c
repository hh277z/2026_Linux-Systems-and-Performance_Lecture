#define _GNU_SOURCE
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define BLOCK_SIZE ((size_t)4096)
#define BLOCK_COUNT ((size_t)16384)
#define MAPPING_SIZE ((size_t)64 * 1024 * 1024)

/* Objects used in Lab 1. Only their addresses are inspected. */
int g_initialized = 7;
unsigned char g_bss[1024 * 1024];
const char g_readonly[] = "Week 7 memory lab";
static void *blocks[BLOCK_COUNT];

void example_function(void)
{
    /* Its address identifies an executable mapping. */
}

/* A newline advances exactly one stage. EOF requests cleanup and failure. */
static int pause_at(int stage, const char *name, size_t live_blocks)
{
    printf("\nSTAGE %d %s pid=%ld live_blocks=%zu live_bytes=%zu\n",
           stage, name, (long)getpid(), live_blocks, live_blocks * BLOCK_SIZE);
    puts("WAITING: inspect in terminal B; press ENTER here to continue.");
    for (;;) {
        int ch = getc(stdin);
        if (ch == '\n')
            return 1;
        if (ch == EOF) {
            fputs("Input ended before the lab finished; cleaning up.\n", stderr);
            return 0;
        }
    }
}

/* Touch every page intersecting this object, including its final byte.
 * malloc addresses need not be page-aligned. volatile preserves the stores. */
static void touch_pages(void *memory, size_t length, size_t page_size)
{
    volatile unsigned char *p = memory;
    size_t next_page = page_size - (uintptr_t)memory % page_size;
    p[0] = 1;
    for (size_t offset = next_page; offset < length; offset += page_size)
        p[offset] = 1;
    p[length - 1] = 1;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        fputs("Usage: ./lab7_memory\n", stderr);
        return EXIT_FAILURE;
    }
    /* Do not defer stage markers when stdout is piped through tee/strace. */
    if (setvbuf(stdout, NULL, _IONBF, 0) != 0 ||
        setvbuf(stdin, NULL, _IONBF, 0) != 0) {
        fputs("Cannot configure standard I/O.\n", stderr);
        return EXIT_FAILURE;
    }
    long page_result = sysconf(_SC_PAGESIZE);
    if (page_result <= 0) {
        fputs("Cannot determine page size.\n", stderr);
        return EXIT_FAILURE;
    }
    size_t page_size = (size_t)page_result;
    int local_variable = 1;
    void *mapping = MAP_FAILED;
    size_t allocated = 0;
    int result = EXIT_FAILURE;

    printf("PID=%ld PAGE_SIZE=%zu BLOCK_SIZE=%zu BLOCK_COUNT=%zu MMAP_BYTES=%zu\n",
           (long)getpid(), page_size, BLOCK_SIZE, BLOCK_COUNT, MAPPING_SIZE);
    printf("OBJECT initialized=%p bss_first=%p bss_last=%p\n",
           (void *)&g_initialized, (void *)g_bss,
           (void *)&g_bss[sizeof g_bss - 1]);
    printf("OBJECT readonly=%p local=%p function=%p pointer_table=%p\n",
           (const void *)g_readonly, (void *)&local_variable,
           (void *)example_function, (void *)blocks);
    if (!pause_at(1, "BASELINE", 0))
        goto cleanup;

    /* Lab 2: many small allocations, then one explicit anonymous mapping. */
    for (size_t i = 0; i < BLOCK_COUNT; i++) {
        blocks[i] = malloc(BLOCK_SIZE);
        if (blocks[i] == NULL) {
            perror("malloc");
            goto cleanup;
        }
        allocated++;
    }
    mapping = mmap(NULL, MAPPING_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        perror("mmap");
        goto cleanup;
    }
    /* Integers remain useful as historical addresses after free/munmap. */
    uintptr_t mapping_start = (uintptr_t)mapping;
    uintptr_t mapping_end = mapping_start + MAPPING_SIZE;
    int increasing = 1;
    for (size_t i = 1; i < BLOCK_COUNT; i++)
        if ((uintptr_t)blocks[i] <= (uintptr_t)blocks[i - 1])
            increasing = 0;
    printf("MALLOC first=%p last=%p addresses_increasing=%d\n",
           blocks[0], blocks[BLOCK_COUNT - 1], increasing);
    printf("MMAP start=0x%" PRIxPTR " end=0x%" PRIxPTR " bytes=%zu\n",
           mapping_start, mapping_end, MAPPING_SIZE);
    if (!pause_at(2, "ALLOCATED_UNTOUCHED", allocated))
        goto cleanup;

    for (size_t i = 0; i < BLOCK_COUNT; i++)
        touch_pages(blocks[i], BLOCK_SIZE, page_size);
    touch_pages(mapping, MAPPING_SIZE, page_size);
    if (!pause_at(3, "TOUCHED", allocated))
        goto cleanup;

    /* Lab 3: retain the final block so its heap placement can prevent
     * returning the preceding free space from the top of the heap. */
    for (size_t i = 0; i + 1 < BLOCK_COUNT; i++) {
        free(blocks[i]);
        blocks[i] = NULL;
    }
    printf("RETAINED last=%p bytes=%zu\n", blocks[BLOCK_COUNT - 1], BLOCK_SIZE);
    if (!pause_at(4, "FREED_KEEP_LAST", 1))
        goto cleanup;

    if (munmap(mapping, MAPPING_SIZE) != 0) {
        perror("munmap");
        goto cleanup;
    }
    mapping = MAP_FAILED;
    printf("UNMAPPED former_start=0x%" PRIxPTR " former_end=0x%" PRIxPTR "\n",
           mapping_start, mapping_end);
    if (!pause_at(5, "UNMAPPED_KEEP_LAST", 1))
        goto cleanup;
    result = EXIT_SUCCESS;

cleanup:
    if (mapping != MAP_FAILED && munmap(mapping, MAPPING_SIZE) != 0) {
        perror("cleanup munmap");
        result = EXIT_FAILURE;
    }
    for (size_t i = 0; i < allocated; i++) {
        free(blocks[i]);
        blocks[i] = NULL;
    }
    puts("CLEANUP complete");
    return result;
}
