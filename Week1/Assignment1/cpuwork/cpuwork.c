/* Instructor-provided CPU workload for week1.
 * make
 * ./cpuwork --threads 1   (also use 2 and 4, one run at a time)
 * Stop with Ctrl-C, or kill -TERM <printed PID> for a background run.
 * N means N computing threads INCLUDING main, not N workers plus main.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_THREADS 64
#define BATCH_SIZE 65536

struct worker {
    unsigned int index;
    uint64_t result;
};

/* Only main receives SIGINT/SIGTERM and reads interrupted.
 * Other workers use the atomic stop flag, not a shared volatile variable. */
static volatile sig_atomic_t interrupted;
static atomic_bool stopping = ATOMIC_VAR_INIT(0);

static void handle_stop(int signo)
{
    (void)signo;
    interrupted = 1;
}

static void *compute(void *arg)
{
    struct worker *w = arg;
    uint64_t value = UINT64_C(0x9e3779b97f4a7c15) ^ (w->index + 1);

    while (!atomic_load_explicit(&stopping, memory_order_relaxed)) {
        /* Each thread computes locally: no I/O, sleeps or shared writes.
         * Unsigned arithmetic makes wraparound well-defined. */
        for (unsigned int i = 0; i < BATCH_SIZE; ++i) {
            value ^= value >> 12;
            value ^= value << 25;
            value ^= value >> 27;
            value *= UINT64_C(2685821657736338717);
        }
        if (w->index == 0 && interrupted)
            break;
    }
    /* Consumed after joining, so the calculation cannot be discarded as dead
     * work. This checksum is not a benchmark score or a reproducible result. */
    w->result = value;
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t threads[MAX_THREADS - 1];
    struct worker workers[MAX_THREADS] = {0};
    struct sigaction action = {0};
    sigset_t stop_signals;
    char *end;
    long count;
    int created = 0;
    int result = EXIT_SUCCESS;
    int error;
    uint64_t checksum = 0;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s --threads N  (1-%d; course experiments: 1, 2, 4)\n",
               argv[0], MAX_THREADS);
        puts("Runs until Ctrl-C or SIGTERM. N includes the main thread.");
        return EXIT_SUCCESS;
    }
    if (argc != 3 || strcmp(argv[1], "--threads") != 0) {
        fprintf(stderr, "Usage: %s --threads N\n", argv[0]);
        return EXIT_FAILURE;
    }
    errno = 0;
    count = strtol(argv[2], &end, 10);
    if (errno != 0 || end == argv[2] || *end != '\0' ||
        count < 1 || count > MAX_THREADS) {
        fprintf(stderr, "threads must be an integer from 1 to %d\n", MAX_THREADS);
        return EXIT_FAILURE;
    }

    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    error = pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        return EXIT_FAILURE;
    }
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    /* Workers inherit blocked stop signals; only main will unblock them. */
    for (int i = 1; i < count; ++i) {
        workers[i].index = (unsigned int)i;
        error = pthread_create(&threads[created], NULL, compute, &workers[i]);
        if (error != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            result = EXIT_FAILURE;
            goto stop;
        }
        ++created;
    }
    error = pthread_sigmask(SIG_UNBLOCK, &stop_signals, NULL);
    if (error != 0) {
        fprintf(stderr, "pthread_sigmask: %s\n", strerror(error));
        result = EXIT_FAILURE;
        goto stop;
    }
    printf("cpuwork: PID=%ld threads=%ld (Ctrl-C or SIGTERM to stop)\n",
           (long)getpid(), count);
    fflush(stdout);
    compute(&workers[0]);

stop:
    atomic_store_explicit(&stopping, 1, memory_order_relaxed);
    for (int i = 0; i < created; ++i) {
        error = pthread_join(threads[i], NULL);
        if (error != 0) {
            fprintf(stderr, "pthread_join: %s\n", strerror(error));
            result = EXIT_FAILURE;
        }
    }
    if (result == EXIT_SUCCESS) {
        for (int i = 0; i < count; ++i)
            checksum ^= workers[i].result;
        printf("cpuwork: stopped, checksum=%016" PRIx64 "\n", checksum);
    }
    return result;
}