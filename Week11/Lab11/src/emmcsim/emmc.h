/*
 * emmcsim: nbdkit plugin that models the timing of a TV eMMC.
 *
 * I/O reaches this plugin through the real Linux block layer and the nbd
 * driver. The plugin keeps the data in RAM and decides when each request
 * completes, using the example values of the Week 11 lecture.
 *
 *   emmcsim.c  nbdkit callbacks, configuration, control socket
 *   model.c    timing: host bus, NAND channel, dies, SRAM buffer, flush
 *   store.c    data: durable NAND copy, volatile versions, image file
 *   ftl.c      simple FTL: page allocation and logical-to-die mapping
 */
#ifndef EMMC_H
#define EMMC_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdio.h>

#define BLK        4096u          /* logical block kept by the store */
#define MAX_DIES   16
#define MAX_PLANES 4
#define NREGION    2              /* 0 = TLC user area, 1 = pSLC (enhanced) area */

enum op_kind { OP_READ, OP_PROG, OP_ERASE, OP_NKIND };
enum lat_kind { LAT_READ, LAT_WRITE, LAT_FLUSH };
#define LAT_N 65536                /* 1 us buckets up to 8 ms, then 64 us buckets up to about 3.7 s */
enum suspend_mode { SUSP_OFF, SUSP_IPS, SUSP_IPC };

/* Every time below is in nanoseconds and already multiplied by time_scale. */
struct cfg {
    uint64_t size;                 /* device size in bytes */
    int dies, planes, multiplane;
    uint32_t page;                 /* NAND page size in bytes */
    uint32_t pages_per_block;
    uint64_t t_r[NREGION], t_prog[NREGION], t_bers[NREGION];
    uint64_t pslc_bytes;           /* LBA range [0, pslc_bytes) is the pSLC area */
    uint32_t host_mbps, nand_mbps; /* host bus and NAND channel bandwidth */
    uint64_t t_cmd;                /* command transfer on the host bus */
    uint32_t buffer_slots;         /* SRAM write buffer, in pages */
    int cache;                     /* 1: volatile write cache, FLUSH/FUA needed */
    int read_priority;             /* 1: reads go ahead of queued programs */
    int suspend;                   /* enum suspend_mode */
    uint64_t suspend_phase;        /* one ISPP phase of a program */
    uint32_t suspend_max;          /* suspensions allowed per operation, 0 = no limit */
    int erase_on_open;             /* optionally model erase when opening a block; default off */
    uint32_t read_retry_pct;       /* % of reads of old data that need one retry */
    double time_scale;
    char conf[256], image[256], save[256], ctl[100];   /* ctl fits sun_path */
    int trace;
};

/* Counters shown by "emmcctl stat". Times are in ns. */
struct stats {
    uint64_t t_reset;
    uint64_t host_reads, host_writes, host_flushes, host_fua, host_trims;
    uint64_t bytes_read, bytes_written;
    uint64_t page_reads, buffer_hits, retries;
    uint64_t prog_ops, pages_programmed, pad_bytes, erases;
    uint64_t die_busy[MAX_DIES][OP_NKIND], die_redo[MAX_DIES], die_susp[MAX_DIES];
    uint64_t bus_busy, chan_busy, slot_stall;
    uint32_t lat[3][LAT_N];        /* device-side latency histogram */
};

extern struct cfg cfg;
extern struct stats st;
extern pthread_mutex_t dev_lock;   /* one lock protects model, store and ftl */
extern int dev_dead;               /* set by powercut: every request fails */

uint64_t now_ns(void);
void sleep_until(uint64_t t);
int region_of(uint64_t lba);       /* lba = 4 KB block number */

/* model.c */
struct waitref { int die; uint64_t op; };
#define MAX_WAIT 64
int  model_init(void);
uint64_t model_read(uint64_t off, uint32_t len, void *buf);           /* returns completion time */
uint64_t model_write(uint64_t off, uint32_t len, const void *buf, int fua,
                     struct waitref *w, int *nw);
uint64_t model_flush(struct waitref *w, int *nw);
uint64_t model_wait_end(const struct waitref *w, int nw);              /* 0 when all are done */
void model_powercut(void);
void model_stat(FILE *f, int raw);
void model_trace(FILE *f, int clear);
void model_reset_stats(void);
void model_lat(int kind, uint64_t ns);

/* store.c */
struct ver {
    uint64_t lba, seq;
    struct ver *next;              /* next version in the same NAND page */
    uint8_t data[BLK];
};
int  store_init(void);
const uint8_t *store_latest(uint64_t lba, int *pending, int *old);
struct ver *store_new_version(uint64_t lba, const void *buf, uint32_t boff, uint32_t len);
void store_commit(struct ver *v, int die);
void store_drop(struct ver *v);
void store_drop_all_pending(void);
int  store_save(const char *path);
int  store_load(const char *path);

/* ftl.c */
struct ppa { int die, plane, need_erase; };
int  ftl_init(void);
int  ftl_lookup(uint64_t lba);     /* die that holds the block */
struct ppa ftl_alloc(int region);
void ftl_commit(uint64_t lba, int die);
void ftl_trim(uint64_t lba);

#endif
