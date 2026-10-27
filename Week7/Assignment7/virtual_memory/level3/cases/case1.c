/* Assignment 7: case 1. Build with the supplied Makefile. */
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
         1);
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
  /* Deliberate loss of ownership: one touched MiB per tick. */
  for (unsigned i = 0; i < ticks; i++) {
    void *p = alloc_bytes(1024 * 1024);
    touch(p, 1024 * 1024);
    sink = (uintptr_t)p;
    phase("running");
    wait_ms(interval);
  }
  phase("done");
  return 0;
}
