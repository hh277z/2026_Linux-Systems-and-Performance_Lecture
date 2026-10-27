/* Assignment 7: case 3. Build with the supplied Makefile. */
#include "common.h"
#include <malloc.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
static atomic_int leave_workers;
static atomic_uint ready_workers;
static unsigned ticks = 12, interval = 1000, threads = 24;
static size_t mib = 32;
static void *idle_worker(void *arg) {
  (void)arg;
  atomic_fetch_add(&ready_workers, 1);
  while (!atomic_load(&leave_workers))
    wait_ms(10);
  return NULL;
}
static void help(void) {
  printf("case%d: --ticks 2..120 --interval-ms 50..5000 --mib 1..64 --threads "
         "1..64\n",
         3);
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
  pthread_t ids[64];
  for (unsigned i = 0; i < threads; i++) {
    if (pthread_create(ids + i, NULL, idle_worker, NULL)) {
      fputs("thread creation failed\n", stderr);
      return 1;
    }
    while (atomic_load(&ready_workers) < i + 1)
      wait_ms(1);
    phase("thread_added");
    wait_ms(interval);
  }
  phase("steady");
  for (unsigned i = 0; i < ticks; i++)
    wait_ms(interval);
  atomic_store(&leave_workers, 1);
  for (unsigned i = 0; i < threads; i++)
    pthread_join(ids[i], NULL);
  phase("done");
  return 0;
}
