#ifndef MTRACE_STUDENT_H
#define MTRACE_STUDENT_H
#include <stddef.h>
#include <stdint.h>
/* Hooks run under the support library's statistics mutex, with instrumentation
 * disabled. Do not call the allocator here. old_addr is an identity only: the
 * allocation may already have been freed. Never dereference it. */
enum mt_op { MT_MALLOC, MT_CALLOC, MT_REALLOC, MT_FREE };
void mt_event(enum mt_op op, uintptr_t old_addr, void *result,
              size_t requested, int overflow);
void mt_report(int fd, const char *reason);
/* Allocation-free output helpers supplied by support.c. */
void mt_text(int fd, const char *text);
void mt_number(int fd, uint64_t value);
#endif
