/* Assignment 7: case 4. Build with the supplied Makefile. */
#include "common.h"
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
static volatile uintptr_t sink;
static unsigned ticks = 12, interval = 1000, threads = 24;
static size_t mib = 32;
static void help(void) {
  printf("case%d: --ticks 2..120 --interval-ms 50..5000 --mib 1..64 --threads "
         "1..64\n",
         4);
  puts("Defaults: ticks=12 interval-ms=1000 mib=32 threads=24.\nWait for READY "
       "before sampling. No input required.\nProcess exit reclaims remaining "
       "storage; SIGTERM stops immediately.\nPeak workload storage is bounded; "
       "see package README.\nOptions irrelevant to this case are accepted but "
       "have no effect.");
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--help")) {
      help();
      return 0;
    }
    if (i + 1 >= argc) {
      fputs("missing option value\n", stderr);
      return 2;
    }
    const char *key = argv[i++];
    if (!strcmp(key, "--ticks"))
      ticks = number(argv[i], 2, 120);
    else if (!strcmp(key, "--interval-ms"))
      interval = number(argv[i], 50, 5000);
    else if (!strcmp(key, "--mib"))
      mib = number(argv[i], 1, 64);
    else if (!strcmp(key, "--threads"))
      threads = number(argv[i], 1, 64);
    else {
      fputs("unknown option\n", stderr);
      return 2;
    }
  }
  phase("baseline");
  wait_ms(interval);
  struct timespec a, b;
  clock_gettime(CLOCK_MONOTONIC, &a);
  for (unsigned i = 0; i < ticks; i++) {
    for (unsigned j = 0; j < 64; j++) {
      void *p = alloc_bytes(mib * 1024 * 1024);
      touch(p, mib * 1024 * 1024);
      sink = (uintptr_t)p;
      free(p);
    }
    phase("batch_complete");
  }
  clock_gettime(CLOCK_MONOTONIC, &b);
  printf("elapsed_ms=%.3f\n",
         (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_nsec - a.tv_nsec) / 1e6);
  phase("done");
  return 0;
}
