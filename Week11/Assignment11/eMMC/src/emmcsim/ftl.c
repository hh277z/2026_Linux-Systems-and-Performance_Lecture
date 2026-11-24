/*
 * ftl.c: the simplest FTL that still behaves like flash.
 *
 * - Page mapping: each 4 KB logical block remembers which die holds it.
 * - New data always goes to the next free page (no overwrite in place).
 * - Pages are striped over planes and dies so that dies work in parallel.
 * - Opening a block can optionally schedule a synthetic erase, controlled by
 *   erase_on_open (off by default). There is no GC: Week 11 uses fresh pages
 *   far below device capacity. Week 12 replaces this file.
 *
 * Interface used by model.c and store.c:
 *   ftl_lookup(lba)      die that holds a logical block
 *   ftl_alloc(region)    die and plane of the next page, and whether the block must be erased first
 *   ftl_commit(lba,die)  a program finished: the block now lives on that die
 *   ftl_trim(lba)        the host no longer needs the block
 */
#include <stdlib.h>
#include <string.h>
#include "emmc.h"

#define UNMAPPED 0xFF

static uint8_t *die_of;
/* per 4 KB block */
static uint64_t next_page[NREGION];

int ftl_init(void)
{
    uint64_t n = cfg.size / BLK;
    die_of = malloc(n);
    if (!die_of)
        return -1;
    memset(die_of, UNMAPPED, n);
    return 0;
}

/* Blocks the host never wrote are treated as written at the factory,
 * one NAND page after another across the dies. */
int ftl_lookup(uint64_t lba)
{
    if (die_of[lba] != UNMAPPED)
        return die_of[lba];
    uint64_t page_no = lba * BLK / cfg.page;
    return (int)(page_no % (uint64_t)cfg.dies);
}

struct ppa ftl_alloc(int region)
{
    uint64_t i = next_page[region]++;
    uint64_t per_row = (uint64_t)cfg.dies * cfg.planes;
    struct ppa p;
    if (cfg.multiplane) {          /* plane 0, plane 1 of die 0, then die 1, ... */
        p.plane = (int)(i % cfg.planes);
        p.die = (int)((i / cfg.planes) % cfg.dies);
    } else {                       /* die 0, die 1, ... then the next plane */
        p.die = (int)(i % cfg.dies);
        p.plane = (int)((i / cfg.dies) % cfg.planes);
    }
    uint64_t page_in_block = (i / per_row) % cfg.pages_per_block;
    p.need_erase = cfg.erase_on_open && page_in_block == 0 && p.plane == 0;
    return p;
}

void ftl_commit(uint64_t lba, int die)
{
    die_of[lba] = (uint8_t)die;
}

void ftl_trim(uint64_t lba)
{
    die_of[lba] = UNMAPPED;
}
