/*
 * logpolicy.h: how a TV service writes its log (Assignment 11, Level 2).
 *
 * The logger calls these functions for every line. The policy decides when
 * lines go from RAM to the file and when they are synced to the device.
 *
 * Rules
 *   - Lines reach the file in the order they were appended.
 *   - An error line (level 1) and every line before it are durable
 *     (written and fsync/fdatasync finished) when lp_append returns.
 *   - An info line (level 0) may stay in RAM, but not longer than
 *     max_delay_ms and not more than buf_kb of lines.
 *   - lp_durable_seq returns the largest seq that is known to be durable.
 *     Never report a line as durable before its sync has returned.
 */
#ifndef LOGPOLICY_H
#define LOGPOLICY_H
#include <stddef.h>
#include <stdint.h>

struct lp_params {
    const char *path;          /* log file, created and truncated by lp_open */
    uint32_t buf_kb;           /* RAM buffer for info lines */
    uint32_t max_delay_ms;     /* longest time an info line may stay only in RAM */
    int use_fdatasync;         /* 1: fdatasync, 0: fsync */
};

struct logpolicy;

struct logpolicy *lp_open(const struct lp_params *p);
int lp_append(struct logpolicy *lp, const char *line, size_t len, int level, uint64_t seq,
    uint64_t now_ns);
int lp_tick(struct logpolicy *lp, uint64_t now_ns);
    /* called after every line and while idle */
uint64_t lp_durable_seq(const struct logpolicy *lp);
uint64_t lp_syncs(const struct logpolicy *lp);
int lp_close(struct logpolicy *lp);                   /* write and sync what is left, close */

#endif
