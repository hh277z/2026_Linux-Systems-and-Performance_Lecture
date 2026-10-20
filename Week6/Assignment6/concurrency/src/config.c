/*
 * config.c - case 4: picture config (Level 3).
 *
 * cfg points to the current config. output_render updates the output's state
 * under the output's lock, then draws the frame with the config, so the
 * config must stay valid until the frame is drawn. config_update makes a new
 * version and applies it to every output. Config updates are rare, frames
 * are very frequent.
 *
 * Run tools/l3_run.sh 4 to see why this file can deadlock, then fix it
 * inside TODO(L3-4). The rest of the file is provided.
 */
#include <stdlib.h>
#include <string.h>

#include "devsvc.h"
#include "rcu.h"

struct output outputs[NOUTPUTS];
struct config *cfg;                     /* the current config */

static struct config *config_alloc(unsigned version)
{
    struct config *c = malloc(sizeof *c);
    if (!c)
        abort();
    c->version = version;
    for (int i = 0; i < CFG_FIELDS; i++)
        c->v[i] = cfg_value(version);
    return c;
}

/* Overwrites the config before free, so a reader that still uses it sees garbage. */
static void config_free(struct config *c)
{
    memset(c, 0xff, sizeof *c);
    free(c);
}

static int config_ok(const struct config *c)
{
    for (int i = 0; i < CFG_FIELDS; i++)
        if (c->v[i] != cfg_value(c->version))
            return 0;
    return 1;
}

void config_init(void)
{
    cfg = config_alloc(1);
    for (int i = 0; i < NOUTPUTS; i++) {
        pthread_mutex_init(&outputs[i].lock, NULL);
        outputs[i].id = i;
        outputs[i].applied = 1;
    }
}

/* ==== TODO(L3-4) begin ==================================================
 * Readers take no lock for cfg: use rcu.h. Free an old config only after a
 * grace period. Keep the output locks, and keep svc_work where it is.
 */

pthread_mutex_t cfg_lock = PTHREAD_MUTEX_INITIALIZER;

int output_render(int out)
{
    struct output *o = &outputs[out];

    pthread_mutex_lock(&o->lock);
    pthread_mutex_lock(&cfg_lock);      /* held until the frame is drawn: c stays valid */
    const struct config *c = cfg;
    int ok = config_ok(c);
    o->frames++;
    o->shown = c->version;
    pthread_mutex_unlock(&o->lock);

    svc_work(WORK_FRAME);               /* draws the frame with c */
    ok = ok && config_ok(c);
    pthread_mutex_unlock(&cfg_lock);
    return ok;
}

void config_update(void)
{
    pthread_mutex_lock(&cfg_lock);
    struct config *old = cfg;
    struct config *c = config_alloc(old->version + 1);
    cfg = c;
    config_free(old);
    for (int i = 0; i < NOUTPUTS; i++) {        /* apply it to every output */
        pthread_mutex_lock(&outputs[i].lock);
        outputs[i].applied = c->version;
        pthread_mutex_unlock(&outputs[i].lock);
    }
    pthread_mutex_unlock(&cfg_lock);
}

unsigned config_version(void)
{
    pthread_mutex_lock(&cfg_lock);
    unsigned v = cfg->version;
    pthread_mutex_unlock(&cfg_lock);
    return v;
}

/* ==== TODO(L3-4) end ==================================================== */
