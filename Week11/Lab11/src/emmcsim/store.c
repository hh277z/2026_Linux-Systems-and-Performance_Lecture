/*
 * store.c: where the data lives.
 *
 * Two layers, as in a device without power-loss protection:
 *   nand[]  durable copy. Only data whose NAND program has finished is here.
 *   pend[]  newest volatile version of a block, still in the SRAM buffer or
 *           in a program that has not finished. A power cut throws it away.
 * A read always sees the newest data: pend[] first, then nand[].
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <nbdkit-plugin.h>
#include "emmc.h"

static uint64_t nblocks;
static uint8_t **nand;
/* NULL = never written (reads as zeros) */
static uint64_t *nand_seq;
/* version number of the durable copy */
static struct ver **pend;
/* newest version not yet durable */
static uint8_t *rewritten;
/* 1 = written since this run started */
static uint64_t gseq;
static const uint8_t zero_block[BLK];

int store_init(void)
{
    nblocks = cfg.size / BLK;
    nand = calloc(nblocks, sizeof *nand);
    nand_seq = calloc(nblocks, sizeof *nand_seq);
    pend = calloc(nblocks, sizeof *pend);
    rewritten = calloc(nblocks, 1);
    if (!nand || !nand_seq || !pend || !rewritten) {
        nbdkit_error("store: out of memory for %lu blocks", (unsigned long)nblocks);
        return -1;
    }
    return 0;
}

/* Newest data of a block. *pending = 1 when it is not durable yet,
 * *old = 1 when the block was not rewritten since the device started. */
const uint8_t *store_latest(uint64_t lba, int *pending, int *old)
{
    if (old)
        *old = !rewritten[lba];
    if (pend[lba]) {
        if (pending)
            *pending = 1;
        return pend[lba]->data;
    }
    if (pending)
        *pending = 0;
    return nand[lba] ? nand[lba] : zero_block;
}

/* A host write of len bytes at byte offset boff inside block lba. */
struct ver *store_new_version(uint64_t lba, const void *buf, uint32_t boff, uint32_t len)
{
    struct ver *v = malloc(sizeof *v);
    if (!v)
        return NULL;
    if (boff != 0 || len != BLK)                  /* partial block: read-modify-write */
        memcpy(v->data, store_latest(lba, NULL, NULL), BLK);
    memcpy(v->data + boff, buf, len);
    v->lba = lba;
    v->seq = ++gseq;
    v->next = NULL;
    pend[lba] = v;
    rewritten[lba] = 1;
    return v;
}

static int all_zero(const uint8_t *p)
{
    for (unsigned i = 0; i < BLK; i += 8) {
        if (*(const uint64_t *)(p + i))
            return 0;
    }
    return 1;
}

/* The NAND program that holds v has finished: v becomes durable.
 * Programs on different dies can finish out of order, so an older version
 * never overwrites a newer durable one. */
void store_commit(struct ver *v, int die)
{
    uint64_t lba = v->lba;
    if (v->seq > nand_seq[lba]) {
        nand_seq[lba] = v->seq;
        if (all_zero(v->data)) {
            free(nand[lba]);
            nand[lba] = NULL;
        } else {
            if (!nand[lba])
                nand[lba] = malloc(BLK);
            if (nand[lba])
                memcpy(nand[lba], v->data, BLK);
        }
        ftl_commit(lba, die);
    }
    if (pend[lba] == v)
        pend[lba] = NULL;
    free(v);
}

void store_drop(struct ver *v)
{
    if (pend[v->lba] == v)
        pend[v->lba] = NULL;
    free(v);
}

void store_drop_all_pending(void)
{
    memset(pend, 0, nblocks * sizeof *pend);
}

/* Durable layer to a sparse raw image (holes for blocks never written). */
int store_save(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -errno;
    if (ftruncate(fd, (off_t)cfg.size) < 0) {
        close(fd);
        return -errno;
    }
    for (uint64_t b = 0; b < nblocks; b++) {
        if (nand[b] && pwrite(fd, nand[b], BLK, (off_t)(b * BLK)) != BLK) {
            close(fd);
            return -EIO;
        }
    }
    fsync(fd);
    close(fd);
    return 0;
}

int store_load(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    off_t end = lseek(fd, 0, SEEK_END), pos = 0;
    uint8_t tmp[BLK];
    while (pos < end) {
        off_t d = lseek(fd, pos, SEEK_DATA);
        if (d < 0)
            break;
            /* Only holes remain. */
        off_t h = lseek(fd, d, SEEK_HOLE);
        for (off_t o = d & ~(off_t)(BLK - 1); o < h && (uint64_t)o < cfg.size; o += BLK) {
            if (pread(fd, tmp, BLK, o) != BLK)
                break;
            uint64_t b = (uint64_t)o / BLK;
            if (!all_zero(tmp)) {
                if (!nand[b])
                    nand[b] = malloc(BLK);
                if (nand[b])
                    memcpy(nand[b], tmp, BLK);
            }
        }
        pos = h;
    }
    close(fd);
    return 0;
}
