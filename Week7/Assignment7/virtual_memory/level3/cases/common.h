#ifndef WORKLOAD_COMMON_H
#define WORKLOAD_COMMON_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static inline void die(const char *s) {
  perror(s);
  exit(1);
}
static inline void *alloc_bytes(size_t n) {
  void *p = malloc(n);
  if (!p)
    die("malloc");
  return p;
}
static inline void touch(void *p, size_t n) {
  volatile unsigned char *q = p;
  long z = sysconf(_SC_PAGESIZE);
  if (z <= 0)
    die("pagesize");
  for (size_t i = 0; i < n; i += (size_t)z)
    q[i] = 1;
  if (n)
    q[n - 1] = 1;
}
static inline void wait_ms(unsigned ms) {
  struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000};
  while (nanosleep(&t, &t) < 0 && errno == EINTR) {
  }
}
static inline void phase(const char *s) {
  printf("PID=%ld READY phase=%s\n", (long)getpid(), s);
  fflush(stdout);
}
static inline void checkpoint(const char *s) {
  phase(s);
  char b[32];
  for (;;) {
    errno = 0;
    if (fgets(b, sizeof b, stdin))
      return;
    if (errno == EINTR) {
      clearerr(stdin);
      continue;
    }
    fprintf(stderr, "checkpoint: stdin closed; provide Enter at each stage\n");
    exit(2);
  }
}
static inline unsigned number(const char *s, unsigned min, unsigned max) {
  char *e;
  errno = 0;
  unsigned long n = strtoul(s, &e, 10);
  if (errno || !*s || *e || n < min || n > max) {
    fprintf(stderr, "invalid number: %s (allowed %u..%u)\n", s, min, max);
    exit(2);
  }
  return (unsigned)n;
}
#endif
