/*
 * logpolicy.c: when log lines go from RAM to the file and the device (Assignment 11, Level 2).
 *
 * Read logpolicy.h first: it is the specification.
 *
 * This skeleton builds and runs: it writes and syncs every line, like logger A on
 * lecture p.52. It is durable, but it costs about as much NAND per line as
 * "logger -m line". Replace the TODO parts so that
 *   - info lines collect in a RAM buffer of buf_kb,
 *   - the buffer is written with one write() and one sync when it is full, when its
 *     oldest line has waited max_delay_ms, or when an error line arrives,
 *   - lp_durable_seq reports only lines whose sync has returned.
 * Keep the function names and signatures.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "logpolicy.h"

struct logpolicy {
    int fd, use_fdatasync;
    uint64_t durable_seq, syncs;
    /* TODO(L2-1): the RAM buffer, how full it is, when its oldest line arrived,
     *             the seq of its newest line, max_delay in ns */
};

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int sync_fd(struct logpolicy *lp)
{
    if ((lp->use_fdatasync ? fdatasync(lp->fd) : fsync(lp->fd)) < 0) {
        return -1;
    }
    lp->syncs++;
    return 0;
}

struct logpolicy *lp_open(const struct lp_params *p)
{
    struct logpolicy *lp = calloc(1, sizeof *lp);
    if (!lp) {
        return NULL;
    }
    lp->fd = open(p->path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (lp->fd < 0) {
        free(lp);
        return NULL;
    }
    lp->use_fdatasync = p->use_fdatasync;
    /* TODO(L2-1): allocate the buffer (p->buf_kb), remember p->max_delay_ms.
     * TODO(L2-4): the new file is durable only when its name is: also sync the directory. */
    return lp;
}

int lp_append(struct logpolicy *lp, const char *line, size_t len, int level, uint64_t seq,
    uint64_t now_ns)
{
    (void)level;
    (void)now_ns;
    /* TODO(L2-2): buffer info lines. Write and sync the buffer when the new line does not fit,
     *             and right after an error line is added (the error line and everything
     *             before it must be durable when this function returns). */
    if (write_all(lp->fd, line, len) < 0 || sync_fd(lp) < 0) {
        return -1;
    }   /* stub: every line */
    lp->durable_seq = seq;
    return 0;
}

int lp_tick(struct logpolicy *lp, uint64_t now_ns)
{
    (void)lp;
    (void)now_ns;
    /* TODO(L2-3): write and sync the buffer if its oldest line has waited max_delay_ms. */
    return 0;
}

uint64_t lp_durable_seq(const struct logpolicy *lp)
{
    return lp->durable_seq;
}
uint64_t lp_syncs(const struct logpolicy *lp)
{
    return lp->syncs;
}

int lp_close(struct logpolicy *lp)
{
    /* TODO(L2-2): write and sync what is left in the buffer, free it. */
    int rc = close(lp->fd);
    free(lp);
    return rc;
}
