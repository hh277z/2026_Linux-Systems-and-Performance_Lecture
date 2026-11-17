#include "lab_common.h"
#include <sys/mman.h>
#include <sys/vfs.h>

static volatile sig_atomic_t running = 1;
static void stop(int signum) { (void)signum; running = 0; }

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "existing") && strcmp(argv[1], "new"))) {
        fputs("Usage: lab10_c existing|new CGROUP\n", stderr); return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL)) { perror("sigaction"); return 1; }
    struct statfs fs;
    if (statfs("/var/tmp", &fs)) { perror("statfs"); return 1; }
    if (fs.f_type == 0x01021994 || fs.f_type == 0x858458f6) {
        fputs("Disk-backed /var/tmp is required; tmpfs/ramfs is unsuitable.\n", stderr); return 1;
    }
    join_group(argv[2]);
    char path[] = "/var/tmp/week10-manual-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    /* Unlink now; the kernel removes the open file even after an OOM/SIGKILL. */
    if (unlink(path)) { perror("unlink"); close(fd); return 1; }
    size_t size = (!strcmp(argv[1], "existing") ? 96 : 192) * MIB;
    if (ftruncate(fd, (off_t)size)) { perror("ftruncate"); close(fd); return 1; }
    void *mapping = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED) { perror("mmap"); return 1; }
    volatile const unsigned char *bytes = mapping;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    printf("PID=%ld ROLE=%s FILE_WORKSET_MIB=%zu CGROUP=%s\n", (long)getpid(), argv[1], size / MIB, argv[2]);
    puts("columns: monotonic_seconds,role,work_ms,checksum");
    while (running) {
        double start = now_seconds();
        unsigned long sum = 0;
        for (size_t i = 0; i < size && running; i += page) sum += bytes[i];
        double end = now_seconds();
        if (running) printf("%.6f,%s,%.6f,%lu\n", end, argv[1], (end - start) * 1000, sum);
        usleep(100000);
    }
    munmap(mapping, size);
    return 0;
}
