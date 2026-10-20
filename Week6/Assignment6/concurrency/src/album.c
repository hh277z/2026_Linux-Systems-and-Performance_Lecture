/*
 * album.c - album data, drawing, and saving (Level 2).
 *
 * Everything here is provided except album_snapshot and album_snapshot_free.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "album.h"
#include "common.h"

void album_init(album_t *a)
{
    memset(a, 0, sizeof *a);
    for (int i = 0; i < ALBUM_ITEMS; i++) {
        char buf[48];
        snprintf(buf, sizeof buf, "track-%03d", i);
        a->items[i].id = i;
        a->items[i].title = strdup(buf);
        a->items[i].rating = 3;
    }
}

void album_free(album_t *a)
{
    for (int i = 0; i < ALBUM_ITEMS; i++) {
        free(a->items[i].title);
        a->items[i].title = NULL;
    }
}

void album_apply(album_t *a, album_update_t *u)
{
    album_item_t *it = &a->items[u->index];
    free(it->title);
    it->title = u->title;       /* the album owns the new title from now on */
    u->title = NULL;
    it->rating = u->rating;
    it->version++;
    a->updates++;
}

uint64_t album_draw(const album_t *a)
{
    uint64_t sum = a->updates;
    for (int i = 0; i < ALBUM_ITEMS; i++) {
        const album_item_t *it = &a->items[i];
        sum = sum * 31 + (uint64_t)it->rating + it->version + strlen(it->title);
    }
    return sum;
}

album_update_t *update_make(unsigned *seed)
{
    album_update_t *u = malloc(sizeof *u);
    char buf[48];
    if (!u)
        return NULL;
    u->index = (int)(rand_r(seed) % ALBUM_ITEMS);
    u->rating = 1 + (int)(rand_r(seed) % 5);
    snprintf(buf, sizeof buf, "track-%03d-rev%u", u->index, rand_r(seed) % 100000);
    u->title = strdup(buf);
    return u;
}

void update_free(album_update_t *u)
{
    if (!u)
        return;
    free(u->title);
    free(u);
}

long album_save(const album_t *a, const char *path, int compress_us, int disk_ms)
{
    /* 1. serialize */
    size_t cap = (size_t)ALBUM_ITEMS * 96, len = 0;
    char *buf = malloc(cap);
    if (!buf)
        return -1;
    for (int i = 0; i < ALBUM_ITEMS; i++) {
        const album_item_t *it = &a->items[i];
        len += (size_t)snprintf(buf + len, cap - len, "%d\t%d\t%llu\t%s\n",
                                it->id, it->rating, (unsigned long long)it->version, it->title);
    }

    /* 2. "compress": CPU work only */
    burn_cpu_us((uint64_t)compress_us);

    /* 3. write and flush to storage */
    long written = -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        written = (long)write(fd, buf, len);
        fsync(fd);
        close(fd);
    }
    free(buf);

    /* 4. slow flash: the device takes disk_ms more to finish */
    struct timespec ts = { .tv_sec = disk_ms / 1000, .tv_nsec = (long)(disk_ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
    return written;
}

/* ---- Level 2 ---------------------------------------------------------- */

/*
 * TODO(L2-1)
 *  Return a deep copy of a: a new album_t whose titles are its own copies.
 *  `*s = *a` alone copies only the title pointers. After the caller unlocks,
 *  a worker may free one of those titles while the saver is still reading it.
 *  Return NULL if memory runs out, and free whatever you allocated before.
 */
album_t *album_snapshot(const album_t *a)
{
    (void)a;
    return NULL;                    /* replace this stub */
}

/* TODO(L2-1): free everything album_snapshot allocated. NULL is allowed. */
void album_snapshot_free(album_t *s)
{
    (void)s;                        /* replace this stub */
}
