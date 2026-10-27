#include "mtrace_student.h"

void mt_event(enum mt_op op, uintptr_t old_addr, void *result,
              size_t requested, int overflow)
{
    (void)op; (void)old_addr; (void)result; (void)requested; (void)overflow;
    /* TODO: count outer API calls, maintain a bounded pointer table, live/peak
     * bytes and blocks, and a successful-request histogram.
     * realloc failure (requested > 0) keeps the old allocation.
     * Target glibc frees a non-NULL pointer on realloc(p, 0).
     * malloc/calloc zero-size non-NULL results remain live blocks.
     * An overflowed calloc call counts, but has no allocation/histogram event.
     * MT_FREE is an event even when old_addr == 0.
     */
}

void mt_report(int fd, const char *reason)
{
    mt_text(fd, "snapshot reason="); mt_text(fd, reason);
    mt_text(fd, " implementation=student-skeleton complete=no\n");
    /* TODO: emit declared accounting mode, API counts, request histogram,
     * live blocks/bytes and peak bytes. Do not use printf/snprintf/malloc.
     * Keep incomplete accounting visible after metadata exhaustion.
     */
}
