#include "lab_common.h"

static volatile sig_atomic_t running = 1;
static void stop(int signum) { (void)signum; running = 0; }

/* Missing/unlimited values are represented as -1, not fabricated zeroes. */
static double metric(const char *directory, const char *file, const char *key)
{
    char path[4096], line[512], name[128];
    int size = snprintf(path, sizeof path, "%s/%s", directory, file);
    if (size < 0 || (size_t)size >= sizeof path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    double value = -1;
    while (fgets(line, sizeof line, f)) {
        if (!key) { if (sscanf(line, "%lf", &value) == 1) break; }
        else {
            double number;
            if (sscanf(line, "%127s %lf", name, &number) == 2 && !strcmp(name, key)) {
                value = number; break;
            }
        }
    }
    fclose(f);
    return value;
}

static double pressure(const char *group, const char *kind)
{
    char path[4096], line[512], name[32];
    int size = snprintf(path, sizeof path, "%s/memory.pressure", group);
    if (size < 0 || (size_t)size >= sizeof path) return -1;
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    double result = -1;
    while (fgets(line, sizeof line, f)) {
        double value;
        if (sscanf(line, "%31s avg10=%lf", name, &value) == 2 && !strcmp(name, kind)) {
            result = value; break;
        }
    }
    fclose(f);
    return result;
}

static void value_text(double value, double unit, const char *suffix)
{
    if (value < 0) printf("n/a");
    else printf("%.1f%s", value / unit, suffix);
}

static void group_line(const char *parent, const char *name, double budget)
{
    char group[4096];
    int size = snprintf(group, sizeof group, "%s/%s", parent, name);
    if (size < 0 || (size_t)size >= sizeof group) return;
    double current = metric(group, "memory.current", NULL);
    if (current < 0) return;
    int bars = budget > 0 ? (int)(current * 24 / budget) : 0;
    if (bars > 24) bars = 24;
    printf("  %-8s [", name);
    for (int i = 0; i < 24; i++) putchar(i < bars ? '#' : '.');
    printf("] %6.1f MiB | anon %.1f file %.1f\n", current/MIB,
           metric(group, "memory.stat", "anon")/MIB, metric(group, "memory.stat", "file")/MIB);
    printf("           limits high=");
    double high = metric(group, "memory.high", NULL);
    if (high < 0) printf("max"); else printf("%.0fMiB", high/MIB);
    printf(" max=");
    double maximum = metric(group, "memory.max", NULL);
    if (maximum < 0) printf("max"); else printf("%.0fMiB", maximum/MIB);
    printf(" | PSI some/full %.2f/%.2f\n", pressure(group, "some"), pressure(group, "full"));
    printf("           events high=%.0f oom_kill=%.0f | ",
           metric(group, "memory.events", "high"), metric(group, "memory.events", "oom_kill"));
    double pid = metric(group, "cgroup.procs", NULL);
    if (pid > 0) {
        char proc[128];
        snprintf(proc, sizeof proc, "/proc/%.0f", pid);
        printf("PID=%.0f PSS=", pid);
        value_text(metric(proc, "smaps_rollup", "Pss:"), 1024, "MiB");
        puts(" (process total)");
    } else puts("no workload PID in this cgroup");
}

static void work_line(const char *path, double now)
{
    FILE *f = fopen(path, "r");
    if (!f) { printf("  %s: waiting for workload log\n", path); return; }
    if (!fseek(f, 0, SEEK_END)) {
        long size = ftell(f);
        if (size > 16384) {
            fseek(f, size - 16384, SEEK_SET);
            char partial[512];
            (void)fgets(partial, sizeof partial, f);
        } else rewind(f);
    }
    char line[512];
    double sum = 0;
    unsigned int count = 0;
    while (fgets(line, sizeof line, f)) {
        double t, ms;
        if (sscanf(line, "%lf,%*[^,],%lf", &t, &ms) == 2 && t >= now - 3 && t <= now) {
            sum += ms; count++;
        }
    }
    fclose(f);
    if (count) printf("  %s: last 3s completed=%u mean work=%.3fms\n", path, count, sum/count);
    else printf("  %s: no completed samples in the last 3s\n", path);
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 4) {
        fputs("Usage: lab10_view PARENT_CGROUP [existing.log [new.log]]\n", stderr); return 2;
    }
    if (metric(argv[1], "memory.current", NULL) < 0) {
        fputs("Cannot read parent cgroup. Check setup and CG.\n", stderr); return 1;
    }
    struct sigaction action = {0};
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL)) return 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("LIVE VIEW | actual sampled metrics | PSI is avg10 percent");
    puts("Bars compare each service's current with the PARENT budget (24 cells).");
    puts("Stop this viewer with Ctrl+C; workloads keep running.");
    double start = now_seconds();
    while (running) {
        double current = metric(argv[1], "memory.current", NULL);
        if (current < 0) { puts("Experiment cgroup removed; viewer finished."); break; }
        double budget = metric(argv[1], "memory.max", NULL);
        printf("\n+ %.1fs | HOST MemAvailable ", now_seconds() - start);
        value_text(metric("/proc", "meminfo", "MemAvailable:"), 1024, "MiB");
        printf("\n  PARENT current "); value_text(current, MIB, "MiB");
        printf(" / "); value_text(budget, MIB, "MiB");
        printf(" | peak "); value_text(metric(argv[1], "memory.peak", NULL), MIB, "MiB");
        printf(" | OOM kills %.0f\n", metric(argv[1], "memory.events", "oom_kill"));
        group_line(argv[1], "media", budget);
        group_line(argv[1], "existing", budget);
        group_line(argv[1], "new", budget);
        for (int i = 2; i < argc; i++) work_line(argv[i], now_seconds());
        sleep(1);
    }
    return 0;
}
