/*
 * lockcheck.c - a small user-space lockdep (provided tool, do not modify).
 *
 *   make bin/lockcheck.so
 *   LD_PRELOAD=./bin/lockcheck.so ./program ...
 *
 * Warnings (each starts with "lockcheck: W<n>")
 *   W1  possible deadlock: taking a lock would close a cycle of waits in the
 *       lock order graph, or the thread takes again a lock it holds
 *   W2  a lock is still held when the program exits normally
 *   W3  a function that may block is called while holding a lock
 *       (a risk warning, not proof that the call actually blocked)
 *   W4  unlock by a thread that does not hold the lock
 * Every warning is counted, but only the first one from the same place in
 * the code is printed (at most 128 places). The summary line at exit gives
 * the counts.
 *
 * Locks it knows
 *   pthread_mutex_t   lock, trylock, unlock, destroy, and the unlock and
 *                     relock inside pthread_cond_wait and timedwait. Normal,
 *                     errorcheck, adaptive and recursive mutexes. Taking a
 *                     recursive mutex again is not a warning.
 *   pthread_rwlock_t  rdlock, wrlock, tryrdlock, trywrlock, unlock, destroy.
 * Not intercepted: pthread_mutex_timedlock, the timed rwlock calls,
 * semaphores, spinlocks. A lock is identified by its address, and destroy
 * forgets it. Only the paths that actually run are seen.
 *
 * How a cycle is judged (the same idea as the kernel's lockdep)
 *   Every edge "held A in mode h, then took B in mode t" is kept with both
 *   modes (read or write, a mutex is always write). A cycle of edges is a
 *   possible deadlock only if every lock on it can make the next thread wait:
 *   at lock L, the thread that wants L waits for the thread that holds L
 *   unless both are readers. Two readers do not wait for each other with
 *   glibc's default rwlock (PTHREAD_RWLOCK_PREFER_READER_NP), so a read-read
 *   step breaks the cycle. With PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
 *   a reader also waits behind a writer that is waiting, so for such a lock a
 *   read-read step does not break the cycle. lockcheck reads the kind of
 *   every rwlock (and the type of every mutex) from glibc's pthread structs.
 *
 * Layout
 *   1. real functions
 *   2. reporting
 *   3. lock table
 *   4. locks held by a thread
 *   5. lock order graph
 *   6. mutex wrappers
 *   7. rwlock wrappers
 *   8. condvar wrappers
 *   9. calls that may block
 *  10. start and exit
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define TLS __thread __attribute__((tls_model("initial-exec")))

enum { READ = 0, WRITE = 1 };

/* ===================================================================== */
/* 1. Real functions                                                     */
/* ===================================================================== */

static int (*real_mutex_lock)(pthread_mutex_t *);
static int (*real_mutex_trylock)(pthread_mutex_t *);
static int (*real_mutex_unlock)(pthread_mutex_t *);
static int (*real_mutex_destroy)(pthread_mutex_t *);
static int (*real_rw_rdlock)(pthread_rwlock_t *);
static int (*real_rw_wrlock)(pthread_rwlock_t *);
static int (*real_rw_tryrdlock)(pthread_rwlock_t *);
static int (*real_rw_trywrlock)(pthread_rwlock_t *);
static int (*real_rw_unlock)(pthread_rwlock_t *);
static int (*real_rw_destroy)(pthread_rwlock_t *);
static int (*real_cond_wait)(pthread_cond_t *, pthread_mutex_t *);
static int (*real_cond_timedwait)(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);
static int (*real_fsync)(int);
static int (*real_nanosleep)(const struct timespec *, struct timespec *);
static int (*real_clock_nanosleep)(clockid_t, int, const struct timespec *, struct timespec *);
static int (*real_usleep)(useconds_t);
static unsigned (*real_sleep)(unsigned);

static void resolve_real(void)
{
    real_mutex_lock      = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_mutex_trylock   = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_mutex_unlock    = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_mutex_destroy   = dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    real_rw_rdlock       = dlsym(RTLD_NEXT, "pthread_rwlock_rdlock");
    real_rw_wrlock       = dlsym(RTLD_NEXT, "pthread_rwlock_wrlock");
    real_rw_tryrdlock    = dlsym(RTLD_NEXT, "pthread_rwlock_tryrdlock");
    real_rw_trywrlock    = dlsym(RTLD_NEXT, "pthread_rwlock_trywrlock");
    real_rw_unlock       = dlsym(RTLD_NEXT, "pthread_rwlock_unlock");
    real_rw_destroy      = dlsym(RTLD_NEXT, "pthread_rwlock_destroy");
    /* glibc keeps two versions of the condvar functions (GLIBC_2.2.5 and
     * GLIBC_2.3.2), and the old one uses another pthread_cond_t layout.
     * dlvsym asks for the current version explicitly. */
    real_cond_wait       = dlvsym(RTLD_NEXT, "pthread_cond_wait", "GLIBC_2.3.2");
    real_cond_timedwait  = dlvsym(RTLD_NEXT, "pthread_cond_timedwait", "GLIBC_2.3.2");
    real_read            = dlsym(RTLD_NEXT, "read");
    real_write           = dlsym(RTLD_NEXT, "write");
    real_fsync           = dlsym(RTLD_NEXT, "fsync");
    real_nanosleep       = dlsym(RTLD_NEXT, "nanosleep");
    real_clock_nanosleep = dlsym(RTLD_NEXT, "clock_nanosleep");
    real_usleep          = dlsym(RTLD_NEXT, "usleep");
    real_sleep           = dlsym(RTLD_NEXT, "sleep");
}

#define RESOLVE(fn) do { if (!(fn)) resolve_real(); } while (0)

/* in_hook is set while lockcheck itself runs. Every wrapper checks it, so
 * anything lockcheck calls goes straight to the real function. */
static TLS int in_hook;
static TLS pid_t my_tid;

static pid_t self_tid(void)
{
    if (!my_tid)
        my_tid = (pid_t)syscall(SYS_gettid);
    return my_tid;
}

/* ===================================================================== */
/* 2. Reporting                                                          */
/* ===================================================================== */

static atomic_int n_warn[5];           /* n_warn[1..4] count W1..W4 */

/* "func+0x1a (prog)" for a code address, or "prog+0x1234" if it has no name.
 * Programs built with -rdynamic export their function names. */
static const char *site_name(void *addr, char *buf, size_t len)
{
    Dl_info di;
    if (addr && dladdr(addr, &di) && di.dli_fname) {
        const char *file = strrchr(di.dli_fname, '/');
        file = file ? file + 1 : di.dli_fname;
        if (di.dli_sname)
            snprintf(buf, len, "%s+0x%lx (%s)", di.dli_sname,
                     (unsigned long)((char *)addr - (char *)di.dli_saddr), file);
        else
            snprintf(buf, len, "%s+0x%lx", file, (unsigned long)((char *)addr - (char *)di.dli_fbase));
    } else {
        snprintf(buf, len, "%p", addr);
    }
    return buf;
}

/* 1 the first time the pair of keys (k1, k2) is seen, 0 afterwards. */
#define MAX_SEEN 128
static struct { const void *k1, *k2; } seen[MAX_SEEN];
static int n_seen;
static atomic_flag seen_busy = ATOMIC_FLAG_INIT;

static int first_time(const void *k1, const void *k2)
{
    int first = 1;
    while (atomic_flag_test_and_set_explicit(&seen_busy, memory_order_acquire))
        sched_yield();
    for (int i = 0; i < n_seen; i++)
        if (seen[i].k1 == k1 && seen[i].k2 == k2)
            first = 0;
    if (first && n_seen < MAX_SEEN) {
        seen[n_seen].k1 = k1;
        seen[n_seen].k2 = k2;
        n_seen++;
    } else if (first) {
        first = 0;                      /* full: count only, do not flood the output */
    }
    atomic_flag_clear_explicit(&seen_busy, memory_order_release);
    return first;
}

/* ===================================================================== */
/* 3. Lock table: address -> index, owner, and order edges               */
/* ===================================================================== */

#define MAX_LOCKS 256
#define TOMBSTONE ((void *)1)

struct lockinfo {
    void *addr;                /* NULL: free, TOMBSTONE: destroyed          */
    int   is_rw;               /* pthread_rwlock_t, else pthread_mutex_t    */
    int   recursive;           /* mutex: may be taken again by its owner    */
    int   read_shares;         /* rwlock: readers never wait for readers    */
    pid_t owner;               /* mutex owner or rwlock writer, 0: none     */
    void *owner_site;          /* where the owner took it                   */
    int   readers;             /* rwlock: read holds now                    */
    void *reader_site;         /* where the last reader took it             */
};

/* One record per combination of modes: combo = held_mode * 2 + new_mode. */
struct edge_rec {
    void *held_site;           /* where a had been taken                    */
    void *new_site;            /* where b was taken                         */
    pid_t tid;
};

/* edges[a][b]: some thread took lock b while it held lock a. Bit `combo` of
 * mask is set once that combination of modes has been seen. */
struct edge {
    unsigned char   mask;
    struct edge_rec rec[4];
};

static struct lockinfo locks[MAX_LOCKS];
static struct edge edges[MAX_LOCKS][MAX_LOCKS];

/* A spinlock protects locks[] and edges[]. lockcheck cannot protect its own
 * state with a pthread mutex: that mutex would be checked too. */
static atomic_flag table_busy = ATOMIC_FLAG_INIT;

static void table_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&table_busy, memory_order_acquire))
        sched_yield();
}

static void table_unlock(void)
{
    atomic_flag_clear_explicit(&table_busy, memory_order_release);
}

static unsigned hash_addr(const void *p)
{
    uintptr_t x = (uintptr_t)p >> 3;
    x ^= x >> 17;
    x *= 0x9e3779b1u;
    return (unsigned)(x ^ (x >> 15)) % MAX_LOCKS;
}

/* Index of lock p in locks[], adding it if it is new. -1 if the table is full.
 * The mode flags are read again every time, from glibc's structs. */
static int lock_index(void *p, int is_rw)
{
    int found = -1, free_slot = -1;
    unsigned h = hash_addr(p);
    table_lock();
    for (int n = 0; n < MAX_LOCKS; n++) {
        int i = (int)((h + (unsigned)n) % MAX_LOCKS);
        if (locks[i].addr == p) {
            found = i;
            break;
        }
        if (locks[i].addr == TOMBSTONE && free_slot < 0)
            free_slot = i;
        if (locks[i].addr == NULL) {
            if (free_slot < 0)
                free_slot = i;
            break;
        }
    }
    if (found < 0 && free_slot >= 0) {
        locks[free_slot] = (struct lockinfo){ .addr = p };
        found = free_slot;
    }
    if (found >= 0) {
        struct lockinfo *l = &locks[found];
        l->is_rw = is_rw;
        if (is_rw) {
            l->recursive = 0;
            l->read_shares = ((pthread_rwlock_t *)p)->__data.__flags != PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP;
        } else {
            l->recursive = (((pthread_mutex_t *)p)->__data.__kind & 3) == PTHREAD_MUTEX_RECURSIVE_NP;
            l->read_shares = 0;
        }
    }
    table_unlock();
    if (found < 0 && first_time("table full", NULL))
        fprintf(stderr, "lockcheck: note: more than %d locks, the rest are not checked\n", MAX_LOCKS);
    return found;
}

/* destroy: forget the lock and every edge that touches it. */
static void lock_forget(void *p)
{
    table_lock();
    unsigned h = hash_addr(p);
    for (int n = 0; n < MAX_LOCKS; n++) {
        int i = (int)((h + (unsigned)n) % MAX_LOCKS);
        if (locks[i].addr == NULL)
            break;
        if (locks[i].addr != p)
            continue;
        locks[i] = (struct lockinfo){ .addr = TOMBSTONE };
        for (int j = 0; j < MAX_LOCKS; j++) {
            edges[i][j].mask = 0;
            edges[j][i].mask = 0;
        }
        break;
    }
    table_unlock();
}

/* The variable name of a global lock, "name+0x40" for a lock inside a
 * global array or struct, or "mutex@0x..." for any other. */
static const char *lock_name(int idx, char *buf, size_t len)
{
    Dl_info di;
    void *p = locks[idx].addr;
    if (dladdr(p, &di) && di.dli_sname && di.dli_saddr) {
        unsigned long off = (unsigned long)((char *)p - (char *)di.dli_saddr);
        if (off == 0)
            snprintf(buf, len, "%s", di.dli_sname);
        else
            snprintf(buf, len, "%s+0x%lx", di.dli_sname, off);
    } else {
        snprintf(buf, len, "%s@%p", locks[idx].is_rw ? "rwlock" : "mutex", p);
    }
    return buf;
}

/* "" for a mutex, " (read)" or " (write)" for an rwlock. */
static const char *mode_name(int idx, int mode)
{
    if (!locks[idx].is_rw)
        return "";
    return mode == READ ? " (read)" : " (write)";
}

/* ===================================================================== */
/* 4. Locks held by this thread                                          */
/* ===================================================================== */

/* A set, not a stack: a thread may release its locks in any order. */
#define MAX_HELD 32

struct held_lock {
    int   idx;                 /* index into locks[]                        */
    int   mode;                /* READ or WRITE                             */
    int   count;               /* > 1: a recursive mutex or a read lock taken again */
    void *site;                /* where this thread took it                 */
};

static TLS struct held_lock held[MAX_HELD];
static TLS int nheld;

static int held_find(int idx)
{
    for (int i = 0; i < nheld; i++)
        if (held[i].idx == idx)
            return i;
    return -1;
}

static void held_add(int idx, int mode, void *site)
{
    int i = held_find(idx);
    if (i >= 0)
        held[i].count++;
    else if (nheld < MAX_HELD)
        held[nheld++] = (struct held_lock){ idx, mode, 1, site };
}

/* Drop one hold of idx. Returns the mode it was held in, or -1 if not held. */
static int held_drop(int idx)
{
    int i = held_find(idx);
    if (i < 0)
        return -1;
    int mode = held[i].mode;
    if (--held[i].count > 0)
        return mode;
    for (; i < nheld - 1; i++)
        held[i] = held[i + 1];
    nheld--;
    return mode;
}

/* ---- reports that need the held set ---------------------------------- */

/* W1: this thread is about to wait for a lock it already holds. */
static void report_self_deadlock(int idx, int mode, int held_mode, void *site)
{
    char a[96], s[128];
    atomic_fetch_add(&n_warn[1], 1);
    if (!first_time("W1 self", site))
        return;
    table_lock();
    lock_name(idx, a, sizeof a);
    table_unlock();
    fprintf(stderr, "lockcheck: W1 possible deadlock: tid %d takes %s%s at %s while it holds it%s\n",
            (int)self_tid(), a, mode_name(idx, mode), site_name(site, s, sizeof s), mode_name(idx, held_mode));
}

/* W1: taking new_idx (mode new_mode) while holding held_idx closes a cycle.
 * path[0..len-1] runs from new_idx to held_idx, and combo[k] gives the modes
 * of the edge from path[k] to path[k+1]. Call with the table locked. */
static void report_cycle(int held_idx, int held_mode, void *held_site, int new_idx, int new_mode,
                         void *new_site, const int *path, const int *combo, int len)
{
    char a[96], b[96], s1[128], s2[128];
    atomic_fetch_add(&n_warn[1], 1);
    if (!first_time(held_site, new_site))      /* the same two lock calls closed a cycle before */
        return;
    fprintf(stderr, "lockcheck: W1 possible circular locking (tid %d)\n", (int)self_tid());
    fprintf(stderr, "  now:     taking %s%s at %s\n", lock_name(new_idx, a, sizeof a),
            mode_name(new_idx, new_mode), site_name(new_site, s1, sizeof s1));
    fprintf(stderr, "           while holding %s%s (taken at %s)\n", lock_name(held_idx, b, sizeof b),
            mode_name(held_idx, held_mode), site_name(held_site, s2, sizeof s2));
    for (int k = 0; k + 1 < len; k++) {
        int x = path[k], y = path[k + 1], c = combo[k];
        const struct edge_rec *e = &edges[x][y].rec[c];
        fprintf(stderr, "  earlier: %s%s taken at %s\n", lock_name(y, a, sizeof a), mode_name(y, c & 1),
                site_name(e->new_site, s1, sizeof s1));
        fprintf(stderr, "           while holding %s%s (taken at %s) by tid %d\n", lock_name(x, b, sizeof b),
                mode_name(x, c >> 1), site_name(e->held_site, s2, sizeof s2), (int)e->tid);
    }
}

/* W2: lock idx is still held at exit. Call with the table locked. */
static void report_held_at_exit(int idx)
{
    char a[96], s[128], path[64];
    struct lockinfo *l = &locks[idx];
    atomic_fetch_add(&n_warn[2], 1);
    if (l->owner) {
        snprintf(path, sizeof path, "/proc/self/task/%d", (int)l->owner);
        int alive = access(path, F_OK) == 0;
        fprintf(stderr, "lockcheck: W2 %s%s is still held at exit by tid %d (%s), taken at %s\n",
                lock_name(idx, a, sizeof a), mode_name(idx, WRITE), (int)l->owner,
                alive ? "thread still running" : "thread has exited", site_name(l->owner_site, s, sizeof s));
    } else {
        fprintf(stderr, "lockcheck: W2 %s (read) is still held at exit by %d reader(s), last taken at %s\n",
                lock_name(idx, a, sizeof a), l->readers, site_name(l->reader_site, s, sizeof s));
    }
}

/* W3: fn may block, and this thread holds the locks in held[]. */
static void report_blocking(const char *fn, void *site)
{
    atomic_fetch_add(&n_warn[3], 1);
    if (!first_time(fn, site))
        return;
    char s[128], a[96], s2[128];
    fprintf(stderr, "lockcheck: W3 %s() called at %s while holding:\n", fn, site_name(site, s, sizeof s));
    for (int i = 0; i < nheld; i++) {
        table_lock();
        lock_name(held[i].idx, a, sizeof a);
        table_unlock();
        fprintf(stderr, "  %s%s (taken at %s)\n", a, mode_name(held[i].idx, held[i].mode),
                site_name(held[i].site, s2, sizeof s2));
    }
}

/* W4: `what` on lock idx by a thread that does not hold it. owner is the
 * recorded owner (mutex or rwlock writer), 0 if there is none. */
static void report_bad_unlock(const char *what, int idx, void *site, pid_t owner)
{
    atomic_fetch_add(&n_warn[4], 1);
    if (!first_time("W4", site))
        return;
    char a[96], s[128];
    table_lock();
    lock_name(idx, a, sizeof a);
    table_unlock();
    if (owner == 0)
        fprintf(stderr, "lockcheck: W4 %s of %s, which this thread does not hold, by tid %d at %s\n",
                what, a, (int)self_tid(), site_name(site, s, sizeof s));
    else
        fprintf(stderr, "lockcheck: W4 %s of %s by tid %d at %s, but its owner is tid %d\n",
                what, a, (int)self_tid(), site_name(site, s, sizeof s), (int)owner);
}

/* ===================================================================== */
/* 5. Lock order graph                                                   */
/* ===================================================================== */

/* At lock idx, does a thread that wants it in mode `want` wait for a thread
 * that holds it in mode `hold`? Readers wait for readers only on a
 * writer-preferring rwlock. */
static int waits(int idx, int want, int hold)
{
    return want == WRITE || hold == WRITE || !locks[idx].read_shares;
}

/*
 * Breadth-first search for the shortest cycle of waits. A state is (lock x,
 * mode `want` in which the previous thread on the path wants x). An edge
 * x -> y kept with modes (h, t) continues the path only if that thread waits
 * at x, waits(x, want, h), and then wants y in mode t. The search ends at
 * `target` if the thread that wants it waits for this thread, which holds it
 * in mode target_hold. On success path[0..*len-1] runs from `from` to
 * `target`, and combo[k] is the edge from path[k] to path[k+1].
 * Call with the table locked: the static arrays are shared.
 */
static int find_path(int from, int from_want, int target, int target_hold, int *path, int *combo, int *len)
{
    static unsigned char visited[MAX_LOCKS][2];
    static short parent[MAX_LOCKS][2];          /* previous state: lock * 2 + want */
    static unsigned char via[MAX_LOCKS][2];     /* combo of the edge into this state */
    static short queue[2 * MAX_LOCKS];
    int head = 0, tail = 0, found = -1;

    memset(visited, 0, sizeof visited);
    visited[from][from_want] = 1;
    parent[from][from_want] = -1;
    queue[tail++] = (short)(from * 2 + from_want);
    while (head < tail && found < 0) {
        int st = queue[head++], x = st / 2, want = st % 2;
        if (x == target) {
            if (waits(x, want, target_hold))
                found = st;
            continue;                           /* never walk through the target */
        }
        for (int y = 0; y < MAX_LOCKS && found < 0; y++) {
            unsigned m = edges[x][y].mask;
            for (int c = 0; m && c < 4; c++) {
                int h = c >> 1, t = c & 1;
                if (!(m & (1u << c)) || !waits(x, want, h) || visited[y][t])
                    continue;
                visited[y][t] = 1;
                parent[y][t] = (short)st;
                via[y][t] = (unsigned char)c;
                queue[tail++] = (short)(y * 2 + t);
            }
        }
    }
    if (found < 0)
        return 0;

    /* Walk back from the target, then reverse. */
    int n = 0, rev[2 * MAX_LOCKS], revc[2 * MAX_LOCKS];
    for (int st = found; st >= 0; st = parent[st / 2][st % 2]) {
        rev[n] = st / 2;
        revc[n] = via[st / 2][st % 2];
        n++;
    }
    for (int k = 0; k < n; k++)
        path[k] = rev[n - 1 - k];
    for (int k = 0; k + 1 < n; k++)
        combo[k] = revc[n - 2 - k];             /* the edge into path[k + 1] */
    *len = n;
    return 1;
}

/* This thread is about to wait for lock idx in mode `mode` (taken at site).
 * For every other lock h it holds, if the edge h -> idx is new for this pair
 * of modes: report a cycle if the new edge closes one, then record the edge. */
static void check_and_add_edges(int idx, int mode, void *site)
{
    table_lock();
    for (int i = 0; i < nheld; i++) {
        int h = held[i].idx, hm = held[i].mode, c = hm * 2 + mode;
        if (h == idx || (edges[h][idx].mask & (1u << c)))
            continue;

        static int path[2 * MAX_LOCKS], combo[2 * MAX_LOCKS];   /* the table lock protects these */
        int len = 0;
        if (find_path(idx, mode, h, hm, path, combo, &len))
            report_cycle(h, hm, held[i].site, idx, mode, site, path, combo, len);

        edges[h][idx].mask |= (unsigned char)(1u << c);
        edges[h][idx].rec[c] = (struct edge_rec){ held[i].site, site, self_tid() };
    }
    table_unlock();
}

/* The thread is about to wait for idx in `mode`. W1 checks happen before it
 * blocks, so the warning is out even if the real call never returns. */
static void before_wait(int idx, int mode, void *site)
{
    int i = held_find(idx);
    if (i < 0) {
        check_and_add_edges(idx, mode, site);
        return;
    }
    /* Taking a lock it already holds. Fine for a recursive mutex, and for a
     * second read hold if readers do not wait behind a waiting writer. */
    int hm = held[i].mode;
    if (locks[idx].recursive)
        return;
    if (locks[idx].is_rw && mode == READ && hm == READ && locks[idx].read_shares)
        return;
    report_self_deadlock(idx, mode, hm, site);
}

/* This thread now holds idx in `mode` (taken at site). */
static void mark_acquired(int idx, int mode, void *site)
{
    table_lock();
    if (mode == WRITE) {
        locks[idx].owner = self_tid();
        locks[idx].owner_site = site;
    } else {
        locks[idx].readers++;
        locks[idx].reader_site = site;
    }
    table_unlock();
    held_add(idx, mode, site);
}

/* This thread releases idx. Returns 0 if it did not hold it, and then the
 * recorded owner goes to *owner for the report. */
static int mark_released(int idx, pid_t *owner)
{
    int i = held_find(idx);
    if (i < 0) {
        table_lock();
        *owner = locks[idx].owner;
        table_unlock();
        return 0;
    }
    int last = held[i].count == 1;
    int mode = held_drop(idx);
    table_lock();
    if (mode == READ)
        locks[idx].readers--;
    else if (last)
        locks[idx].owner = 0;
    table_unlock();
    return 1;
}

/* ===================================================================== */
/* 6. Mutex wrappers                                                     */
/* ===================================================================== */

int pthread_mutex_lock(pthread_mutex_t *m)
{
    RESOLVE(real_mutex_lock);
    if (in_hook)
        return real_mutex_lock(m);

    void *site = __builtin_return_address(0);
    in_hook = 1;
    int idx = lock_index(m, 0);
    if (idx >= 0)
        before_wait(idx, WRITE, site);
    in_hook = 0;

    int rc = real_mutex_lock(m);

    /* Record ownership only after the real lock succeeded. */
    if (rc == 0 && idx >= 0) {
        in_hook = 1;
        mark_acquired(idx, WRITE, site);
        in_hook = 0;
    }
    return rc;
}

/* A trylock never waits, so it adds no edge. On success it is held. */
int pthread_mutex_trylock(pthread_mutex_t *m)
{
    RESOLVE(real_mutex_trylock);
    int rc = real_mutex_trylock(m);
    if (in_hook || rc != 0)
        return rc;
    in_hook = 1;
    int idx = lock_index(m, 0);
    if (idx >= 0)
        mark_acquired(idx, WRITE, __builtin_return_address(0));
    in_hook = 0;
    return rc;
}

/* Update the records before the real unlock, while this thread still holds it. */
int pthread_mutex_unlock(pthread_mutex_t *m)
{
    RESOLVE(real_mutex_unlock);
    if (in_hook)
        return real_mutex_unlock(m);

    void *site = __builtin_return_address(0);
    in_hook = 1;
    int idx = lock_index(m, 0);
    pid_t owner = 0;
    if (idx >= 0 && !mark_released(idx, &owner))
        report_bad_unlock("unlock", idx, site, owner);
    in_hook = 0;
    return real_mutex_unlock(m);
}

int pthread_mutex_destroy(pthread_mutex_t *m)
{
    RESOLVE(real_mutex_destroy);
    if (!in_hook)
        lock_forget(m);
    return real_mutex_destroy(m);
}

/* ===================================================================== */
/* 7. rwlock wrappers                                                    */
/* ===================================================================== */

static int rw_lock(pthread_rwlock_t *l, int mode, void *site)
{
    int (*real)(pthread_rwlock_t *) = mode == READ ? real_rw_rdlock : real_rw_wrlock;
    if (in_hook)
        return real(l);

    in_hook = 1;
    int idx = lock_index(l, 1);
    if (idx >= 0)
        before_wait(idx, mode, site);
    in_hook = 0;

    int rc = real(l);

    if (rc == 0 && idx >= 0) {
        in_hook = 1;
        mark_acquired(idx, mode, site);
        in_hook = 0;
    }
    return rc;
}

static int rw_trylock(pthread_rwlock_t *l, int mode, void *site)
{
    int rc = (mode == READ ? real_rw_tryrdlock : real_rw_trywrlock)(l);
    if (in_hook || rc != 0)
        return rc;
    in_hook = 1;
    int idx = lock_index(l, 1);
    if (idx >= 0)
        mark_acquired(idx, mode, site);
    in_hook = 0;
    return rc;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *l)
{
    RESOLVE(real_rw_rdlock);
    return rw_lock(l, READ, __builtin_return_address(0));
}

int pthread_rwlock_wrlock(pthread_rwlock_t *l)
{
    RESOLVE(real_rw_wrlock);
    return rw_lock(l, WRITE, __builtin_return_address(0));
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *l)
{
    RESOLVE(real_rw_tryrdlock);
    return rw_trylock(l, READ, __builtin_return_address(0));
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *l)
{
    RESOLVE(real_rw_trywrlock);
    return rw_trylock(l, WRITE, __builtin_return_address(0));
}

/* One unlock releases this thread's hold in whatever mode it took it. */
int pthread_rwlock_unlock(pthread_rwlock_t *l)
{
    RESOLVE(real_rw_unlock);
    if (in_hook)
        return real_rw_unlock(l);

    void *site = __builtin_return_address(0);
    in_hook = 1;
    int idx = lock_index(l, 1);
    pid_t owner = 0;
    if (idx >= 0 && !mark_released(idx, &owner))
        report_bad_unlock("unlock", idx, site, owner);
    in_hook = 0;
    return real_rw_unlock(l);
}

int pthread_rwlock_destroy(pthread_rwlock_t *l)
{
    RESOLVE(real_rw_destroy);
    if (!in_hook)
        lock_forget(l);
    return real_rw_destroy(l);
}

/* ===================================================================== */
/* 8. Condition variable wrappers                                        */
/* ===================================================================== */

/* glibc releases and re-takes m inside pthread_cond_wait without calling
 * pthread_mutex_unlock or pthread_mutex_lock, so the wrappers above never see
 * it. Record the release before the wait and the re-acquisition after it.
 * Re-taking m while holding other locks orders them like a normal lock. */
static int before_cond_wait(pthread_mutex_t *m, void *site)
{
    int idx = lock_index(m, 0);
    pid_t owner = 0;
    if (idx >= 0 && !mark_released(idx, &owner))
        report_bad_unlock("pthread_cond_wait", idx, site, owner);
    return idx;
}

static void after_cond_wait(int idx, void *site)
{
    if (idx < 0)
        return;
    check_and_add_edges(idx, WRITE, site);
    mark_acquired(idx, WRITE, site);
}

int pthread_cond_wait(pthread_cond_t *cv, pthread_mutex_t *m)
{
    RESOLVE(real_cond_wait);
    if (in_hook)
        return real_cond_wait(cv, m);

    void *site = __builtin_return_address(0);
    in_hook = 1;
    int idx = before_cond_wait(m, site);
    in_hook = 0;

    int rc = real_cond_wait(cv, m);

    in_hook = 1;
    after_cond_wait(idx, site);
    in_hook = 0;
    return rc;
}

int pthread_cond_timedwait(pthread_cond_t *cv, pthread_mutex_t *m, const struct timespec *abstime)
{
    RESOLVE(real_cond_timedwait);
    if (in_hook)
        return real_cond_timedwait(cv, m, abstime);

    void *site = __builtin_return_address(0);
    in_hook = 1;
    int idx = before_cond_wait(m, site);
    in_hook = 0;

    int rc = real_cond_timedwait(cv, m, abstime);   /* m is held again, even on ETIMEDOUT */

    in_hook = 1;
    after_cond_wait(idx, site);
    in_hook = 0;
    return rc;
}

/* ===================================================================== */
/* 9. Calls that may block                                               */
/* ===================================================================== */

static void check_blocking(const char *fn, void *site)
{
    if (in_hook || nheld == 0)
        return;
    in_hook = 1;
    report_blocking(fn, site);
    in_hook = 0;
}

ssize_t read(int fd, void *buf, size_t n)
{
    RESOLVE(real_read);
    check_blocking("read", __builtin_return_address(0));
    return real_read(fd, buf, n);
}

ssize_t write(int fd, const void *buf, size_t n)
{
    RESOLVE(real_write);
    check_blocking("write", __builtin_return_address(0));
    return real_write(fd, buf, n);
}

int fsync(int fd)
{
    RESOLVE(real_fsync);
    check_blocking("fsync", __builtin_return_address(0));
    return real_fsync(fd);
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    RESOLVE(real_nanosleep);
    check_blocking("nanosleep", __builtin_return_address(0));
    return real_nanosleep(req, rem);
}

int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
{
    RESOLVE(real_clock_nanosleep);
    check_blocking("clock_nanosleep", __builtin_return_address(0));
    return real_clock_nanosleep(clk, flags, req, rem);
}

int usleep(useconds_t us)
{
    RESOLVE(real_usleep);
    check_blocking("usleep", __builtin_return_address(0));
    return real_usleep(us);
}

unsigned sleep(unsigned s)
{
    RESOLVE(real_sleep);
    check_blocking("sleep", __builtin_return_address(0));
    return real_sleep(s);
}

/* ===================================================================== */
/* 10. Start and exit                                                    */
/* ===================================================================== */

__attribute__((constructor)) static void lockcheck_init(void)
{
    resolve_real();
    fprintf(stderr, "lockcheck: loaded\n");
}

/* Runs at a normal exit only: a killed program never gets here. */
__attribute__((destructor)) static void lockcheck_exit(void)
{
    in_hook = 1;
    fflush(stdout);                     /* the program's output comes before the summary */
    table_lock();
    for (int i = 0; i < MAX_LOCKS; i++)
        if (locks[i].addr && locks[i].addr != TOMBSTONE && (locks[i].owner || locks[i].readers > 0))
            report_held_at_exit(i);
    table_unlock();

    fprintf(stderr, "lockcheck: summary W1=%d W2=%d W3=%d W4=%d\n", atomic_load(&n_warn[1]),
            atomic_load(&n_warn[2]), atomic_load(&n_warn[3]), atomic_load(&n_warn[4]));
}
