/* cpumgr: CPU 자원관리기 (Level 2) 스켈레톤
 * 제공: 옵션 파싱, 파일 읽기/쓰기 helper, cpu.stat 필드 읽기, CSV 헤더, 종료 처리 뼈대
 * 구현(TODO): cgroup 생성과 controller 활성화, 설정 적용, group 안에서 명령 실행, 1초 sampling과 --at 이벤트, schedstat 합산, teardown
 * usage: sudo cpumgr [--weight W] [--max QUOTA PERIOD] [--cpuset CPUS] [--bg WEIGHT]
 *                    [--at SEC:weight=W | SEC:max=Q,P | SEC:cpuset=CPUS]... [--log FILE] -- command args...
 *   group: $CG_ROOT/cpumgr/<manager-pid> (명령), $CG_ROOT/cpumgr/bg-<manager-pid> (background 부하). CG_ROOT 기본값 /sys/fs/cgroup */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 32
struct event { int sec; char key[8]; char value[64]; };   /* key: weight | max | cpuset, value: 파일에 쓸 문자열 */

static char root[256] = "/sys/fs/cgroup", base[320], job[320], bg[320];
static pid_t child = -1, bgpid = -1;
static volatile sig_atomic_t stopping;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* 문자열을 파일에 쓴다 (제공). cgroup 파일은 열어서 한 번에 쓰면 된다. */
static int write_file(const char *path, const char *s) {
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = write(fd, s, strlen(s));
    int rc = n == (ssize_t)strlen(s) ? 0 : -1;
    close(fd);
    return rc;
}

/* 파일 내용을 읽어 끝의 개행을 지운다 (제공) */
static int read_file(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        buf[0] = 0;
        return -1;
    }
    ssize_t n = read(fd, buf, len - 1);
    int saved_errno = errno;
    close(fd);
    if (n < 0) { buf[0] = 0; errno = saved_errno; return -1; }
    buf[n] = 0;
    while (n > 0 && buf[n - 1] == '\n') {
        buf[--n] = 0;
    }
    return 0;
}

/* group/file 에 value를 쓴다 (제공). 모든 정책 변경은 이 함수 하나로 이루어진다. */
static int setting(const char *group, const char *file, const char *value) {
    char path[400];
    snprintf(path, sizeof path, "%s/%s", group, file);
    if (write_file(path, value)) {
        fprintf(stderr, "cpumgr: write %s <- '%s': %s\n", path, value, strerror(errno));
        return -1;
    }
    return 0;
}

/* cpu.stat에서 field 값을 읽는다 (제공). 예: stat_field(job, "nr_throttled") */
static uint64_t stat_field(const char *group, const char *field) {
    char path[400], buf[1024], *p;
    snprintf(path, sizeof path, "%s/cpu.stat", group);
    if (read_file(path, buf, sizeof buf)) { perror(path); return UINT64_MAX; }
    p = strstr(buf, field);
    if (!p) { fprintf(stderr,"missing cpu.stat field: %s\n",field); return UINT64_MAX; }
    return strtoull(p + strlen(field), NULL, 10);
}

/* TODO: $root/cpumgr 와 그 아래 job (want_bg이면 bg도) 를 만들고 cpu, cpuset controller를 켠다.
 *   1) $root/cgroup.subtree_control 에 "+cpu +cpuset"
 *   2) mkdir base, base/cgroup.subtree_control 에 "+cpu +cpuset"
 *   3) mkdir job (, bg). 실패를 확인하며 새 group에만 적용한다.
 * cpuset.cpus는 비어 있으면 부모의 effective 값을 상속한다. 적용 결과는 effective 파일로 확인한다.
 * 성공하면 0, 실패하면 이유를 출력하고 -1. main이 teardown 후 종료한다.
 * root 권한, cgroup v2, systemd=true 필요. */
static int setup_groups(int want_bg) {
    (void)want_bg;
    /* TODO */
    fprintf(stderr,"setup_groups: TODO not implemented\n");
    return -1;
}

/* 제공: 종료를 요청하고 최대 약 2초 기다린 뒤 필요하면 강제 종료한다.
 * exec를 하지 않는 burner도 반드시 기본 signal 동작을 사용해야 한다. */
static int terminate_child(pid_t *pid) {
    if (*pid <= 0) return 0;
    pid_t target = *pid;
    if (kill(target, SIGTERM) && errno != ESRCH) return -1;
    for (int i = 0; i < 100; i++) {
        pid_t r = waitpid(target, NULL, WNOHANG);
        if (r == target || (r < 0 && errno == ECHILD)) { *pid = -1; return 0; }
        if (r < 0 && errno != EINTR) return -1;
        struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL);
    }
    if (kill(target, SIGKILL) && errno != ESRCH) return -1;
    pid_t r; do { r = waitpid(target, NULL, 0); } while (r < 0 && errno == EINTR);
    if (r == target || (r < 0 && errno == ECHILD)) { *pid = -1; return 0; }
    return -1;
}

/* TODO: terminate_child(&child), terminate_child(&bgpid)로 자식들을 먼저 종료시키고 job, bg, base group을 rmdir로 지운다.
 * Ctrl-C로 끝낼 때도 반드시 호출한다. rmdir 실패도 확인한다.
 * base는 다른 cpumgr 실행이 사용 중이면 ENOTEMPTY를 허용한다. 초기 설정 실패 시에도 정리한다. */
static void teardown(void) {
    /* TODO */
}

static void on_signal(int sig) {
    (void)sig;
    stopping = 1;
}

/* TODO: /proc/<pid>/task/<tid>/schedstat 의 첫 두 값(exec ns, run_delay ns)을 모든 thread에 대해 합한다.
 * 제공 dengine은 반복 전체에서 worker를 유지한다. TID별 이전 값을 기억해 증가량을 합산한다.
 * 읽기 실패를 0 사용량으로 해석하지 말고 경고한다. TID 테이블 용량 초과도 오류로 처리한다.
 * polling만으로 sample 사이에 종료된 thread의 마지막 시간은 복구할 수 없다.
 * 정상 종료 전 마지막 sample을 기록하고, 종료 경계 누락 가능성을 보고서에 명시한다. */
static void schedstat_delta(pid_t pid, uint64_t *dexec, uint64_t *ddelay) {
    (void)pid;
    *dexec = *ddelay = 0;
    /* TODO */
}

/* background 부하용 busy loop (제공). 두 thread가 CPU를 계속 쓴다. */
static void *burn(void *p) {
    (void)p;
    volatile uint64_t x = 1;
    for (;;) {
        x = x * 6364136223846793005ull + 1;
    }
    return NULL;
}

/* TODO: fork 하고, 자식은 group/cgroup.procs 에 자기 PID를 쓴 뒤
 *   builtin_burner 이면 burn() thread 하나 + 자기 자신도 burn()
 *   아니면 execvp(argv[0], argv)
 * 부모는 자식 PID를 돌려준다. 자식이 group에 들어간 뒤에 exec 해야 dengine의 모든 thread가 그 group 안에 생긴다. */
static pid_t spawn_in_group(const char *group, char **argv, int builtin_burner) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        /* 제공: 부모 handler를 상속한 builtin burner가 SIGTERM을 무시하지 않도록 초기화. */
        struct sigaction sa = {0};
        sa.sa_handler = SIG_DFL; sigemptyset(&sa.sa_mask);
        if (sigaction(SIGINT, &sa, NULL) || sigaction(SIGTERM, &sa, NULL)) _exit(127);
        (void)group; (void)argv; (void)builtin_burner;
        /* TODO: cgroup.procs에 자신의 PID 기록. 실패하면 stderr와 _exit(127).
         * builtin_burner: pthread_create로 burn thread 하나 + burn(NULL).
         * 일반 command: execvp(argv[0],argv). 실패하면 perror와 _exit(127). */
        fprintf(stderr, "spawn_in_group: TODO not implemented\n");
        _exit(127);
    }
    return pid;
}

int main(int argc, char **argv) {
    const char *weight = NULL, *cpuset = NULL, *logfile = NULL, *bgweight = NULL;
    char maxv[64] = "";
    struct event ev[MAX_EVENTS];
    int nev = 0, i = 1;
    if (getenv("CG_ROOT")) {
        snprintf(root, sizeof root, "%s", getenv("CG_ROOT"));
    }
    /* 옵션 파싱 (제공) */
    for (; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "--weight") && i + 1 < argc) {
            weight = argv[++i];
        } else if (!strcmp(argv[i], "--max") && i + 2 < argc) {
            snprintf(maxv, sizeof maxv, "%s %s", argv[i + 1], argv[i + 2]);
            i += 2;
        } else if (!strcmp(argv[i], "--cpuset") && i + 1 < argc) {
            cpuset = argv[++i];
        } else if (!strcmp(argv[i], "--bg") && i + 1 < argc) {
            bgweight = argv[++i];
        } else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            logfile = argv[++i];
        } else if (!strcmp(argv[i], "--at") && i + 1 < argc && nev < MAX_EVENTS) {
            char *spec = argv[++i], *eq = strchr(spec, '=');
            if (sscanf(spec, "%d:%7[a-z]", &ev[nev].sec, ev[nev].key) != 2 || !eq) {
                fprintf(stderr, "bad --at %s\n", spec);
                return 1;
            }
            snprintf(ev[nev].value, sizeof ev[nev].value, "%s", eq + 1);
            for (char *c = ev[nev].value; *c; c++) {
                if (!strcmp(ev[nev].key, "max") && *c == ',') {
                    *c = ' ';                 /* max=Q,P -> "Q P" (cpu.max 파일 형식) */
                }
            }
            if (ev[nev].sec < 1 || (strcmp(ev[nev].key,"weight") && strcmp(ev[nev].key,"max") && strcmp(ev[nev].key,"cpuset"))) {
                fprintf(stderr,"--at requires seconds >= 1 and weight|max|cpuset\n"); return 1;
            }
            nev++;
        } else {
            fprintf(stderr, "usage: %s [--weight W] [--max Q P] [--cpuset CPUS] [--bg WEIGHT] [--at SEC:weight=W|SEC:max=Q,P|SEC:cpuset=C]... [--log FILE] -- cmd args\n", argv[0]);
            return 1;
        }
    }
    if (i >= argc - 1) {
        fprintf(stderr, "cpumgr: missing -- command\n");
        return 1;
    }
    char **cmd = argv + i + 1;
    snprintf(base, sizeof base, "%s/cpumgr", root);
    snprintf(job, sizeof job, "%s/cpumgr/%ld", root, (long)getpid());
    snprintf(bg, sizeof bg, "%s/cpumgr/bg-%ld", root, (long)getpid());

    if (setup_groups(bgweight != NULL)) { teardown(); return 2; }
    /* TODO: 초기 설정 적용. weight -> job/cpu.weight, maxv -> job/cpu.max, cpuset -> job/cpuset.cpus, bgweight -> bg/cpu.weight */

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    FILE *log = logfile ? fopen(logfile, "w") : stdout;
    if (!log) {
        perror(logfile);
        teardown();
        return 1;
    }
    /* TODO: bgweight가 있으면 bg group에서 burner를 띄우고(bgpid), job group에서 cmd를 실행한다(child) */
    (void)cmd;
    if (child <= 0 || (bgweight && bgpid <= 0)) {
        fprintf(stderr,"command/background launch TODO not implemented or failed\n");
        teardown(); if (log != stdout) fclose(log); return 2;
    }
    fprintf(log, "# cpumgr pid=%d child=%d job=%s\n", (int)getpid(), (int)child, job);
    fprintf(log, "sec,mono_ns,interval_ms,usage_ms,nr_throttled_delta,throttled_ms,exec_ms,run_delay_ms,weight,max,cpuset_effective,event\n");
    fflush(log);

    /* TODO: 1초마다 아래를 반복한다. child가 끝나거나 stopping이면 종료.
     *   - mono_ns=now_ns(), 실제 interval_ms를 기록한다. 먼저 직전 구간을 직전 설정값으로 sample한다.
     *   - cpu.stat의 usage_usec, throttled_usec 증가량은 ms로, nr_throttled 증가량은 count로 출력
     *   - --at 시각 이상이 된 미적용 이벤트를 적용한다(정확히 == 비교하지 않는다).
     *     변경 이벤트 timestamp와 새 값을 별도 # event 줄에 기록. setting() 실패는 nonzero 종료.
     *   - schedstat_delta()로 exec, run_delay 증가량
     *   - job/cpu.weight, job/cpu.max, job/cpuset.cpus.effective 현재값
     *   - cpuset 목록에 comma가 있으면 CSV 필드를 quote한다. 한 줄 출력 후 fflush
     *   - waitpid 결과를 보존하고 child가 끝나면 child=-1. child 실패를 부모 exit status로 전달.
     *   - controller/설정 read 실패를 성공으로 처리하지 않는다. stat_field의 UINT64_MAX는 읽기 실패다.
     * TODO 미완료로 loop를 생략했다면 아래 기본 실패 상태를 그대로 유지한다. */
    int result = 2; /* TODO: child 성공 시 0, 실패/미완료/중단 시 nonzero */
    uint64_t t0 = now_ns();
    (void)t0; (void)ev; (void)nev; (void)weight; (void)maxv; (void)cpuset; (void)bgweight;

    teardown();
    if (log != stdout) fclose(log);
    return result;
}
