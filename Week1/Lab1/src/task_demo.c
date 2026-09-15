#include "lab.h"
#include <pthread.h>
#include <sys/wait.h>

/* Fork a process tree BEFORE creating threads in any process. */
enum { TASKS = 2, PROCESSES = 4 };

typedef struct {
    int role;
    pid_t pid, ppid;
    long tids[TASKS];
} Report;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int ready, released;
static long tids[TASKS];
static int indices[TASKS] = {0, 1};

static void must(int rc, const char *what) {
    if (rc) {
        errno = rc;
        die(what);
    }
}

static void transfer(int fd, void *buffer, size_t length, int writing) {
    char *p = buffer;
    while (length) {
        ssize_t n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            if (!n) {
                errno = EPIPE;
            }
            die("coordination pipe");
        }
        p += n;
        length -= (size_t)n;
    }
}

static void *worker(void *arg) {
    int index = *(int *)arg;

    must(pthread_mutex_lock(&lock), "lock");
    tids[index] = tid(); /* KEY each thread gets its own Linux TID */
    ++ready;
    must(pthread_cond_broadcast(&changed), "broadcast");
    while (!released) {
        must(pthread_cond_wait(&changed, &lock), "wait");
    }
    must(pthread_mutex_unlock(&lock), "unlock");
    return NULL;
}

static void show_status(pid_t pid, long thread_id) {
    char path[128], line[512];
    snprintf(path, sizeof path, "/proc/%ld/task/%ld/status", (long)pid, thread_id);
    printf("%s\n", path);
    FILE *f = fopen(path, "r"); /* KEY procfs read path */
    if (!f) {
        printf("UNAVAILABLE: %s\n", strerror(errno));
        return;
    }
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Tgid:", 5) || !strncmp(line, "Pid:", 4) ||
            !strncmp(line, "PPid:", 5) || !strncmp(line, "Threads:", 8)) {
            fputs(line, stdout);
        }
    }
    fclose(f);
}

static void reap(pid_t child) {
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            die("waitpid");
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs("Child failed\n", stderr);
        exit(1);
    }
}

int main(int argc, char **argv) {
    int automatic = argc == 2 && !strcmp(argv[1], "--auto");
    if (argc > 2 || (argc == 2 && !automatic)) {
        fprintf(stderr, "Usage: %s [--auto]\n", argv[0]);
        return 2;
    }

    int reports[2], gate[2];
    if (pipe(reports) || pipe(gate)) {
        die("pipe");
    }
    int role = 0;
    pid_t child_a = -1, child_b = -1, grandchild = -1;
    child_a = fork(); /* KEY ROOT creates A */
    if (child_a < 0) {
        die("fork A");
    }
    if (!child_a) {
        role = 1;
        grandchild = fork(); /* KEY A creates GRANDCHILD */
        if (grandchild < 0) {
            die("fork grandchild");
        }
        if (!grandchild) {
            role = 3;
        }
    } else {
        child_b = fork(); /* KEY ROOT creates B */
        if (child_b < 0) {
            die("fork B");
        }
        if (!child_b) {
            role = 2;
        }
    }

    /* Each process has one MAIN thread and one WORKER thread. */
    pthread_t direct[TASKS - 1];
    tids[0] = tid();

    for (int i = 0; i < TASKS - 1; ++i) {
        must(pthread_create(&direct[i], NULL, worker, &indices[i + 1]),
             "pthread_create"); /* KEY main creates one worker */
    }

    must(pthread_mutex_lock(&lock), "lock");
    while (ready != TASKS - 1) {
        must(pthread_cond_wait(&changed, &lock), "wait ready");
    }
    must(pthread_mutex_unlock(&lock), "unlock");

    Report mine = {.role = role, .pid = getpid(), .ppid = getppid()};
    memcpy(mine.tids, tids, sizeof tids);
    if (role) {
        close(reports[0]);
        close(gate[1]);
        /* One write smaller than PIPE_BUF keeps reports from interleaving. */
        ssize_t n;
        do {
            n = write(reports[1], &mine, sizeof mine);
        } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof mine) {
            die("report write");
        }
        close(reports[1]);
        char token;
        transfer(gate[0], &token, 1, 0);
        close(gate[0]);
    } else {
        close(reports[1]);
        close(gate[0]);
        Report all[PROCESSES];
        all[0] = mine;
        for (int i = 1; i < PROCESSES; ++i) {
            Report r;
            transfer(reports[0], &r, sizeof r, 0);
            if (r.role < 1 || r.role >= PROCESSES) {
                die("invalid role");
            }
            all[r.role] = r;
        }
        close(reports[0]);

        const char *names[] = {"ROOT", "A", "B", "GRANDCHILD"};
        const char *thread_names[] = {"MAIN", "WORKER"};
        puts("Process tree: ROOT -> A -> GRANDCHILD; ROOT -> B");
        puts("In EACH process: MAIN creates one WORKER. Total: 4 processes, 8 threads "
             "including MAIN threads.");

        int ok = all[1].ppid == all[0].pid && all[2].ppid == all[0].pid &&
                 all[3].ppid == all[1].pid;
        for (int p = 0; p < PROCESSES; ++p) {
            printf("PROCESS role=%s pid=%ld ppid=%ld threads=%d\n", names[p],
                   (long)all[p].pid, (long)all[p].ppid, TASKS);
            ok = ok && all[p].tids[0] == (long)all[p].pid;
            for (int t = 0; t < TASKS; ++t) {
                printf("TASK process=%s role=%s pid=%ld tid=%ld\n", names[p],
                       thread_names[t], (long)all[p].pid, all[p].tids[t]);
                for (int q = 0; q <= p; ++q) {
                    for (int u = 0; u < TASKS; ++u) {
                        if (q < p || u < t) {
                            ok = ok && all[p].tids[t] != all[q].tids[u];
                        }
                    }
                }
                show_status(all[p].pid, all[p].tids[t]);
            }
        }

        printf("TREE_CHECK processes=4 tasks=8 relations_ok=%d\n", ok);
        checkpoint(automatic,
                   "All 8 tasks are alive. Inspect with ps/pstree, then press Enter.");
        char tokens[] = "RRR";
        transfer(gate[1], tokens, 3, 1);
        close(gate[1]);
        if (!ok) {
            return 1;
        }
    }

    must(pthread_mutex_lock(&lock), "lock");
    released = 1;
    must(pthread_cond_broadcast(&changed), "broadcast release");
    must(pthread_mutex_unlock(&lock), "unlock");

    for (int i = 0; i < TASKS - 1; ++i) {
        must(pthread_join(direct[i], NULL), "join");
    }
    if (role == 1) {
        reap(grandchild);
    }
    if (role == 0) {
        reap(child_a);
        reap(child_b);
        puts("All workers joined and descendants reaped.");
    }
    return 0;
}
