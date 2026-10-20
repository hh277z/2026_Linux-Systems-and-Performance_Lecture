/*
 * catalog.c - case 3: app catalog (Level 3).
 *
 * apps[] says which permissions every app holds (apps_lock), perms[] says
 * which permissions are enabled (perms_lock). The catalog is looked up from
 * both sides:
 *   catalog_can_launch  app -> its permission     reads both
 *   catalog_perm_users  permission -> its apps    reads both
 *   catalog_grant       writes apps, reads perms
 *   catalog_revoke      writes apps
 *   catalog_set_perm    writes perms, reads apps
 * Most requests only read. The rule in devsvc.h must hold at every instant:
 * an app never holds a disabled permission.
 *
 * Run tools/l3_run.sh 3 to see why this file can deadlock, then fix it
 * inside TODO(L3-3). The rest of the file is provided.
 */
#include "devsvc.h"

static struct { unsigned perms; } apps[NAPPS];      /* bit p: holds perm p */
static struct { int enabled; } perms[NPERMS];

static int has(int app, int perm)
{
    return (apps[app].perms >> perm) & 1u;
}

/* ==== TODO(L3-3) begin ==================================================
 * Change the two locks to pthread_rwlock_t and take each one in the mode
 * that the path needs. Initialize every rwlock with svc_rwlock_init().
 * Keep the order of the two read-only paths as it is (catalog_can_launch:
 * apps_lock first, catalog_perm_users: perms_lock first), and keep every
 * svc_work inside the locks it is under now.
 */

pthread_mutex_t apps_lock, perms_lock;

void catalog_init(void)
{
    pthread_mutex_init(&apps_lock, NULL);
    pthread_mutex_init(&perms_lock, NULL);
    for (int p = 0; p < NPERMS; p++)
        perms[p].enabled = 1;
}

int catalog_can_launch(int app, int perm)
{
    pthread_mutex_lock(&apps_lock);
    pthread_mutex_lock(&perms_lock);
    int ok = has(app, perm) && perms[perm].enabled;
    svc_work(WORK_LOOKUP);
    pthread_mutex_unlock(&perms_lock);
    pthread_mutex_unlock(&apps_lock);
    return ok;
}

int catalog_perm_users(int perm, int *enabled)
{
    int n = 0;
    pthread_mutex_lock(&perms_lock);
    pthread_mutex_lock(&apps_lock);
    *enabled = perms[perm].enabled;
    for (int a = 0; a < NAPPS; a++)
        n += has(a, perm);
    svc_work(WORK_LOOKUP);
    pthread_mutex_unlock(&apps_lock);
    pthread_mutex_unlock(&perms_lock);
    return n;
}

int catalog_grant(int app, int perm)
{
    pthread_mutex_lock(&apps_lock);
    pthread_mutex_lock(&perms_lock);
    int ok = perms[perm].enabled;
    if (ok)
        apps[app].perms |= 1u << perm;
    svc_work(WORK_LOOKUP);
    pthread_mutex_unlock(&perms_lock);
    pthread_mutex_unlock(&apps_lock);
    return ok;
}

void catalog_revoke(int app, int perm)
{
    pthread_mutex_lock(&apps_lock);
    apps[app].perms &= ~(1u << perm);
    svc_work(WORK_LOOKUP);
    pthread_mutex_unlock(&apps_lock);
}

int catalog_set_perm(int perm, int enabled)
{
    int ok = 1;
    pthread_mutex_lock(&perms_lock);
    pthread_mutex_lock(&apps_lock);
    if (enabled) {
        perms[perm].enabled = 1;
    } else {
        int users = 0;
        for (int a = 0; a < NAPPS; a++)
            users += has(a, perm);
        if (users == 0)
            perms[perm].enabled = 0;
        else
            ok = 0;                     /* still held: refuse */
    }
    svc_work(WORK_LOOKUP);
    pthread_mutex_unlock(&apps_lock);
    pthread_mutex_unlock(&perms_lock);
    return ok;
}

/* ==== TODO(L3-3) end ==================================================== */
