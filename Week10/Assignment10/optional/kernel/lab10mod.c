// SPDX-License-Identifier: GPL-2.0
/* Bounded diagnosis workloads for the disposable course VM. */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/debugfs.h>
#include <linux/workqueue.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>

#define OBJECT_BYTES 256U
#define BATCH_OBJECTS 1024U
#define LAYOUT_ORDER 5U
#define PROBE_ORDER 4U

static unsigned int scenario = 5;
static unsigned int limit_mib = 8;
static unsigned int interval_ms = 1000;
static unsigned int probe_limit = 60;
static bool retain = true;

module_param(scenario, uint, 0444);
module_param(limit_mib, uint, 0444);
module_param(interval_ms, uint, 0444);
module_param(probe_limit, uint, 0444);
module_param(retain, bool, 0444);
MODULE_PARM_DESC(retain, "M5 retain successful allocations until unload");
MODULE_PARM_DESC(probe_limit, "M4 number of order-4 allocation probes");

static void **objects;
static struct page **pages;
static unsigned int capacity;
static unsigned int live_objects;
static unsigned int page_slots;
static unsigned int live_pages;
static unsigned int released_pages;
static unsigned int layout_blocks;
static unsigned int layout_failures;
static u64 cycles;
static u64 allocation_attempts;
static u64 allocation_successes;
static u64 allocation_failures;
static u64 free_count;
static u64 probe_attempts;
static u64 probe_successes;
static u64 probe_failures;
static bool layout_prepared;
static bool workload_done;
static bool stopping;
static struct dentry *dir;
static DEFINE_MUTEX(state_lock);

static void workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(tick, workfn);

static void prepare_layout(void) {
    const unsigned int block_pages = 1U << LAYOUT_ORDER;

    /* split_page gives known physical adjacency WITHIN each acquired block.
     * It does not constrain free blocks elsewhere in the system. */
    while (capacity - page_slots >= block_pages) {
        struct page *block;
        unsigned int start = page_slots;
        unsigned int i;

        block = alloc_pages(GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY, LAYOUT_ORDER);
        if (!block) {
            layout_failures++;
            break;
        }

        split_page(block, LAYOUT_ORDER);
        for (i = 0; i < block_pages; i++)
            pages[page_slots++] = block + i;

        live_pages += block_pages;
        layout_blocks++;

        /* Keep every odd page as an unmovable kernel-owned page. */
        for (i = start; i < page_slots; i += 2) {
            __free_page(pages[i]);
            pages[i] = NULL;
            live_pages--;
            released_pages++;
        }
    }

    /* A preparation failure is recorded once, not silently retried each tick. */
    layout_prepared = true;
}

static void run_probe(void) {
    struct page *page;

    page = alloc_pages(GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY, PROBE_ORDER);
    probe_attempts++;
    if (page) {
        probe_successes++;
        __free_pages(page, PROBE_ORDER);
    } else {
        probe_failures++;
    }

    workload_done = probe_attempts >= probe_limit;
}

static void run_object_batch(void) {
    unsigned int batch;

    /* Both retain modes have the SAME finite number of allocation attempts.
     * Failures consume an attempt as well and remain visible in status. */
    for (batch = 0; batch < BATCH_OBJECTS && allocation_attempts < capacity; batch++) {
        void *object;

        allocation_attempts++;
        object = kmalloc(OBJECT_BYTES, GFP_KERNEL | __GFP_NOWARN | __GFP_NORETRY);
        if (!object) {
            allocation_failures++;
            continue;
        }

        allocation_successes++;
        memset(object, 1, OBJECT_BYTES);
        if (retain) {
            objects[live_objects++] = object;
        } else {
            kfree(object);
            free_count++;
        }
    }

    workload_done = allocation_attempts >= capacity;
}

static void workfn(struct work_struct *work) {
    (void)work;
    mutex_lock(&state_lock);

    if (stopping || workload_done) {
        mutex_unlock(&state_lock);
        return;
    }

    cycles++;
    if (scenario == 5) {
        run_object_batch();
    } else {
        if (!layout_prepared)
            prepare_layout();

        run_probe();
    }

    /* Serialize requeueing with the stop flag set by module exit. */
    if (!workload_done)
        schedule_delayed_work(&tick, msecs_to_jiffies(interval_ms));

    mutex_unlock(&state_lock);
}

static int status_show(struct seq_file *s, void *unused) {
    (void)unused;
    mutex_lock(&state_lock);
    seq_printf(s, "scenario=%u retain=%d capacity=%u cycles=%llu done=%d\n", scenario, retain,
               capacity, (unsigned long long)cycles, workload_done);

    if (scenario == 4) {
        seq_printf(s,
                   "page_size=%lu page_slots=%u live_pages=%u live_payload_bytes=%llu "
                   "released_pages=%u layout_blocks=%u layout_failures=%u\n",
                   (unsigned long)PAGE_SIZE, page_slots, live_pages,
                   (unsigned long long)live_pages * PAGE_SIZE, released_pages, layout_blocks,
                   layout_failures);
        seq_printf(s,
                   "probe_order=%u probe_attempts=%llu probe_successes=%llu "
                   "probe_failures=%llu probe_limit=%u\n",
                   PROBE_ORDER, (unsigned long long)probe_attempts,
                   (unsigned long long)probe_successes, (unsigned long long)probe_failures,
                   probe_limit);
    } else {
        seq_printf(s,
                   "object_bytes=%u allocation_attempts=%llu allocation_successes=%llu "
                   "allocation_failures=%llu free_count=%llu live_objects=%u "
                   "live_payload_bytes=%llu\n",
                   OBJECT_BYTES, (unsigned long long)allocation_attempts,
                   (unsigned long long)allocation_successes,
                   (unsigned long long)allocation_failures, (unsigned long long)free_count,
                   live_objects, (unsigned long long)live_objects * OBJECT_BYTES);
    }

    mutex_unlock(&state_lock);
    return 0;
}

DEFINE_SHOW_ATTRIBUTE(status);

static int __init begin(void) {
    if ((scenario != 4 && scenario != 5) || limit_mib < 1 || limit_mib > 64 || interval_ms < 100 ||
        interval_ms > 60000 || probe_limit < 1 || probe_limit > 3600)
        return -EINVAL;

    capacity = (limit_mib * 1024U * 1024U) / (scenario == 5 ? OBJECT_BYTES : PAGE_SIZE);
    if (scenario == 4 && capacity < (1U << LAYOUT_ORDER))
        return -EINVAL;

    /* Metadata need not itself require a large physically contiguous allocation. */
    if (scenario == 5)
        objects = kvcalloc(capacity, sizeof(*objects), GFP_KERNEL);
    else
        pages = kvcalloc(capacity, sizeof(*pages), GFP_KERNEL);

    if (!objects && !pages)
        return -ENOMEM;

    dir = debugfs_create_dir("lab10", NULL);
    if (IS_ERR_OR_NULL(dir)) {
        kvfree(objects);
        kvfree(pages);
        return -ENODEV;
    }

    if (IS_ERR_OR_NULL(debugfs_create_file("status", 0400, dir, NULL, &status_fops))) {
        debugfs_remove_recursive(dir);
        kvfree(objects);
        kvfree(pages);
        return -ENODEV;
    }

    schedule_delayed_work(&tick, msecs_to_jiffies(interval_ms));
    pr_info("LAB10 bounded module scenario=%u limit_mib=%u\n", scenario, limit_mib);
    return 0;
}

static void __exit end(void) {
    unsigned int i;

    mutex_lock(&state_lock);
    stopping = true;
    mutex_unlock(&state_lock);
    cancel_delayed_work_sync(&tick);
    debugfs_remove_recursive(dir);

    if (objects) {
        for (i = 0; i < live_objects; i++)
            kfree(objects[i]);

        kvfree(objects);
    }

    if (pages) {
        for (i = 0; i < page_slots; i++) {
            if (pages[i])
                __free_page(pages[i]);
        }

        kvfree(pages);
    }

    pr_info("LAB10 module released all owned allocations\n");
}

module_init(begin);
module_exit(end);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded optional Assignment 10 kernel diagnosis workloads");
