/*
 * album.h - the shared state of albumsvc (Level 2).
 *
 * An album is a fixed array of items. Each item owns a heap string (title),
 * so copying an album_t with `*dst = *src` copies only the title pointers.
 */
#ifndef ALBUM_H
#define ALBUM_H

#include <stdint.h>

#define ALBUM_ITEMS 256

typedef struct {
    int      id;
    char    *title;        /* heap string owned by the album that holds this item */
    int      rating;
    uint64_t version;      /* incremented on every update of this item */
} album_item_t;

typedef struct {
    album_item_t items[ALBUM_ITEMS];
    uint64_t     updates;  /* number of updates applied so far */
} album_t;

/* One change produced by a scanner worker. */
typedef struct {
    int   index;           /* which item */
    char *title;           /* new title (heap). Ownership moves to the album in album_apply */
    int   rating;
} album_update_t;

void album_init(album_t *a);
void album_free(album_t *a);                    /* frees the titles, not the album_t itself */

/* Applies u to a. Frees the item's old title and takes u->title (sets it to NULL).
 * Does not free u itself. */
void album_apply(album_t *a, album_update_t *u);

/* Reads every item, as drawing the album view would. Returns a checksum. */
uint64_t album_draw(const album_t *a);

album_update_t *update_make(unsigned *seed);    /* a random update, allocated on the heap */
void update_free(album_update_t *u);            /* frees u and u->title if it is still set */

/*
 * Saves a to `path`: serialize, "compress" (compress_us of CPU work), then
 * write(), fsync(), and nanosleep(disk_ms) to emulate slow flash storage.
 * Returns the number of bytes written, or -1.
 */
long album_save(const album_t *a, const char *path, int compress_us, int disk_ms);

/* ---- Level 2 ---------------------------------------------------------- */

/* TODO(L2-1): a deep copy of a. The result owns all of its memory, titles
 * included, and stays valid after the caller releases whatever protects a. */
album_t *album_snapshot(const album_t *a);

/* TODO(L2-1): frees a snapshot made by album_snapshot. */
void album_snapshot_free(album_t *s);

#endif /* ALBUM_H */
