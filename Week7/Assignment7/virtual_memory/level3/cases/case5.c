/* Assignment 7: case 5. Build with the supplied Makefile. */
#include "common.h"
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
static unsigned ticks = 12, interval = 1000, threads = 24;
static size_t mib = 32;
static void help(void) {
  printf("case%d: --ticks 2..120 --interval-ms 50..5000 --mib 1..64 --threads "
         "1..64\n",
         5);
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
  size_t bytes = mib * 1024 * 1024;
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    die("mmap");
  phase("reserved");
  for (unsigned i = 0; i < ticks; i++)
    wait_ms(interval);
  touch(p, bytes / 4);
  phase("phase2");
  for (unsigned i = 0; i < ticks; i++)
    wait_ms(interval);
  touch(p, bytes);
  phase("phase3");
  for (unsigned i = 0; i < ticks; i++)
    wait_ms(interval);
  if (munmap(p, bytes))
    die("munmap");
  phase("done");
  return 0;
}
