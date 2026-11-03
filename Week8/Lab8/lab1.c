#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/resource.h>

static void die(const char *s) {
    perror(s);
    exit(1);
}

static void target(void) {
    puts("target called normally");
}

static void maps(void *p) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[1024];
    unsigned long a, b;

    if (!f)
        die("maps");

    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%lx-%lx", &a, &b) == 2 &&
            (uintptr_t)p >= a && (uintptr_t)p < b)
            fputs(line, stdout);

    fclose(f);
}

static void pause_stage(int automatic) {
    if (!automatic) {
        puts("Inspect maps above; Enter to continue.");

        if (getchar() == EOF)
            exit(2);
    }
}

int main(int argc, char **argv) {
    int automatic = argc == 3 && !strcmp(argv[2], "--auto");

    if (argc < 2 || argc > 3 || (argc == 3 && !automatic) ||
        (strcmp(argv[1], "text") && strcmp(argv[1], "cow"))) {
        fprintf(stderr, "usage: %s text|cow [--auto]\n", argv[0]);
        return 2;
    }

    setbuf(stdout, NULL);
    struct rlimit core = {0, 0};

    if (setrlimit(RLIMIT_CORE, &core))
        die("setrlimit");

    long ps = sysconf(_SC_PAGESIZE);

    if (ps <= 0)
        die("pagesize");

    if (!strcmp(argv[1], "text")) {
        target();
        volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)target;
        printf("TEXT address=%p\n", (void *)p);
        maps((void *)p);
        pause_stage(automatic);
        pid_t child = fork();

        if (child < 0)
            die("fork");

        if (!child) {
            /* KEY: writing to an RX code page causes SIGSEGV. */
            *p = 0;
            _exit(99);
        }

        int st;

        if (waitpid(child, &st, 0) < 0)
            die("waitpid");

        if (WIFSIGNALED(st))
            printf("child signal=%d expected=%d (SIGSEGV)\n", WTERMSIG(st), SIGSEGV);
        else
            printf("unexpected child exit=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);

        return !(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    }

    size_t n = (size_t)ps * 4096;
    volatile unsigned char *p = mmap(NULL, n, PROT_READ|PROT_WRITE,
                                     MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED)
        die("mmap");

    if (madvise((void *)p, n, MADV_NOHUGEPAGE))
        die("MADV_NOHUGEPAGE");

    /* KEY: touch before fork so resident private pages can be shared. */
    for (size_t i=0; i<n; i+=(size_t)ps)
        p[i]=7;

    printf("COW parent=%ld address=%p pages=4096 initial=7\n", (long)getpid(), (void *)p);
    maps((void *)p);
    pause_stage(automatic);
    pid_t child = fork();

    if (child < 0)
        die("fork");

    if (!child) {
        struct rusage a,b,c;

        maps((void *)p);

        if (getrusage(RUSAGE_SELF, &a))
            die("getrusage");

        /* KEY: the child first write triggers COW; the VMA stays writable. */
        for (size_t i=0; i<n; i+=(size_t)ps)
            p[i]=9;

        if (getrusage(RUSAGE_SELF, &b))
            die("getrusage");

        /* KEY: these pages are now private to the child, so no new COW. */
        for (size_t i=0; i<n; i+=(size_t)ps)
            p[i]=10;

        if (getrusage(RUSAGE_SELF, &c))
            die("getrusage");

        printf("child first-write minor=%ld major=%ld; rewrite minor=%ld major=%ld value=%u\n",
               b.ru_minflt-a.ru_minflt,b.ru_majflt-a.ru_majflt,
               c.ru_minflt-b.ru_minflt,c.ru_majflt-b.ru_majflt,p[0]);

        _exit(0);
    }

    int st;

    if (waitpid(child, &st, 0) < 0)
        die("waitpid");

    printf("parent value=%u (expected 7)\n",p[0]);
    int ok = WIFEXITED(st) && WEXITSTATUS(st)==0 && p[0]==7;

    if (munmap((void *)p,n))
        die("munmap");

    return !ok;
}
