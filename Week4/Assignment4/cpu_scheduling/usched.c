/* usched: 시간 할당량 기반 user-space scheduler (Level 3) 스켈레톤
 * 제공: job 파일 파싱, job/worker 구조체, 출력 형식(요약 표, CSV), 데이터 준비
 * 구현(TODO): ready queue, 도착 시각에 job 투입, worker loop(policy, quantum, block 경계 선점, 재투입), overhead 측정
 * job 파일: 한 줄에 "arrival_ms base_blocks weight name".
 * base_blocks는 항상 256KB 단위. -b와 관계없이 job의 총 작업량을 유지한다.
 * 짧은 job은 base_blocks=4 (1MB), -b 4096에서는 1MB partial block 한 번 처리.
 * usage: usched -p fifo|rr|wrr -q quantum_ms -b block_KB -w workers -j jobfile [-n 64] [-o csv] */
#include "work.h"
#include <getopt.h>
#include <errno.h>

#define MAX_JOBS 256

struct job {
    char name[32];
    uint64_t arrival_ns, length, weight;
    uint64_t base_blocks, total_records, done_records, sum, kept;                                /* 입력 */
    uint64_t done_blocks, cpu_ns, first_start_ns, completion_ns, preemptions, max_turn_ns;   /* 기록 */
    int finished;
    struct job *next;                                                   /* ready queue 연결용 */
};

static struct job jobs[MAX_JOBS];
static int njobs, policy;            /* policy: 0 fifo, 1 rr, 2 wrr */
static uint64_t quantum_ns, t0;
static uint32_t *data;
static size_t nrec, block_recs;
static const int digit = FILTER_RESIDUE; /* filter residue, fixed to 3 in the assignment */
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER;
static struct job *qhead, *qtail;    /* ready queue (단순 연결 list) */
static int finished_jobs, stop;
static FILE *turnlog;
static pthread_mutex_t trace_lock = PTHREAD_MUTEX_INITIALIZER;

/* 제공: 다음 실행 block의 span. 데이터 끝 또는 job 끝에서는 partial block을 반환한다.
 * job의 논리 record 순서는 data[0..nrec)를 반복한다. job끼리 input만 공유한다. */
static size_t job_block_span(const struct job *j, size_t *off) {
    if (j->done_records >= j->total_records) { *off = 0; return 0; }
    *off = (size_t)(j->done_records % nrec);
    uint64_t left = j->total_records - j->done_records;
    size_t n = block_recs < nrec-*off ? block_recs : nrec-*off;
    return left < n ? (size_t)left : n;
}
/* 제공: turn 로그. j의 cpu_ns를 갱신한 후, requeue 전에 호출한다.
 * 출력 비용도 bookkeeping CPU time에 포함한다. 같은 job은 한 worker만 처리한다.
 * 공통 runnable 구간의 CPU share 분석에 사용하며, 경계를 가로지른 turn은 별도로 다룬다. */
static void trace_turn(const struct job *j, uint64_t begin, uint64_t end, uint64_t cpu) {
    pthread_mutex_lock(&trace_lock);
    fprintf(turnlog,"%s,%.6f,%.6f,%.6f,%.6f,%llu,%llu\n",j->name,(begin-t0)/1e6,(end-t0)/1e6,
            cpu/1e6,j->cpu_ns/1e6,(unsigned long long)j->done_records,(unsigned long long)j->weight);
    pthread_mutex_unlock(&trace_lock);
}

/* TODO: qlock을 쥔 상태에서 호출. j를 queue 꼬리에 넣고 기다리는 worker를 깨운다. */
static void enqueue(struct job *j) {
    (void)j;
    /* TODO */
}

/* TODO: qlock을 쥔 상태에서 호출. queue 머리의 job을 떼어 돌려준다. 비어 있으면 NULL. */
static struct job *dequeue(void) {
    /* TODO */
    return NULL;
}

/* TODO: 도착 시각 순으로 정렬된 jobs[]를 돌며, 각 job의 arrival 시각이 되면 enqueue 한다.
 * 짧게 잠들며 기다리고(nanosleep), 마지막 job까지 넣으면 종료. */
static void *releaser(void *p) {
    (void)p;
    set_thread_name("release", 0);
    /* TODO */
    return NULL;
}

struct warg { int id; uint64_t turns, overhead_ns, work_ns; };

/* TODO: worker loop.
 * 1) qlock 아래에서 queue가 빌 동안 qcond로 대기. stop이고 queue가 비면 종료.
 * 2) dequeue한 job의 첫 시작 시각 기록. dequeue 후 qlock을 풀고 계산한다.
 * 3) budget: fifo=무제한, rr=quantum_ns, wrr=quantum_ns*weight/100.
 * 4) turn 시작 CPU time과 wall time 기록. job_block_span(j,&off)의 n record를
 *    work_block(data+off,NULL,n,digit,&j->sum,&j->kept)로 처리한다.
 *    done_records+=n, done_blocks++. CPU time이 budget에 도달하면 block 경계에서 중단.
 *    sum/kept는 job에 누적하며 requeue할 때 초기화하지 않는다.
 * 5) turn CPU time을 j->cpu_ns에 더하고 max_turn_ns 갱신.
 *    trace_turn(j,turn_wall_start,turn_wall_end,turn_cpu_ns)을 requeue 전에 호출.
 *    qlock 아래에서 완료면 finished=1, completion_ns 기록, finished_jobs++.
 *    미완료면 preemptions++ 후 enqueue. 모두 완료되면 stop=1과 broadcast.
 * 6) 순수 work_block 호출 전후 CPU time 차이만 a->work_ns에 더한다.
 *    quantum 판단은 전체 turn CPU time을 사용한다. clock 호출/진행 상태 갱신/queue/
 *    trace 출력/종료 bookkeeping CPU time은 a->overhead_ns에 포함한다.
 *    worker 시작~종료의 CPU time에서 work CPU time을 뺄 수 있다.
 *    condition wait의 wall time은 overhead CPU time에 포함하지 않는다.
 * 같은 job을 동시에 여러 worker에 배정하지 않는다. */
static void *worker(void *p) {
    struct warg *a = p;
    set_thread_name("usched-w", a->id);
    /* TODO */
    return NULL;
}

static int cmp_arrival(const void *x, const void *y) {
    const struct job *a = x, *b = y;
    return a->arrival_ns < b->arrival_ns ? -1 : a->arrival_ns > b->arrival_ns;
}

int main(int argc, char **argv) {
    const double mb = 64;
    int quantum_ms = 20, block_kb = 256, workers = 1, opt;
    const char *jobfile = NULL, *csv = "usched.csv";
    policy = 1;
    while ((opt = getopt(argc, argv, "p:q:b:w:j:n:o:h")) != -1) {
        switch (opt) {
        case 'p': policy = !strcmp(optarg, "fifo") ? 0 : !strcmp(optarg, "rr") ? 1 : !strcmp(optarg, "wrr") ? 2 : -1; break;
        case 'q': quantum_ms = atoi(optarg); break;
        case 'b': block_kb = atoi(optarg); break;
        case 'w': workers = atoi(optarg); break;
        case 'j': jobfile = optarg; break;
        case 'n': if (strcmp(optarg,"64")) { fprintf(stderr,"input is fixed to 64 MB\n"); return 1; } break;
        case 'o': csv = optarg; break;
        default: jobfile = NULL; goto usage;
        }
    }
    if (!jobfile || policy < 0 || workers < 1 || workers > 16 || quantum_ms < 1 || quantum_ms > 10000 || block_kb < 4 || block_kb > 65536 || optind != argc) {
usage:
        fprintf(stderr, "usage: %s -p fifo|rr|wrr -q quantum_ms -b block_KB -w workers -j jobfile [-n 64] [-o csv]\n", argv[0]);
        return 1;
    }
    /* job 파일 파싱 (제공) */
    FILE *f = fopen(jobfile, "r");
    if (!f) {
        perror(jobfile);
        return 1;
    }
    char line[256]; int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *q = line; while (*q == ' ' || *q == '\t') q++;
        if (*q == '#' || *q == '\n' || !*q) continue;
        unsigned long long arr, len, w; char name[32], extra;
        if (*q == '-' || sscanf(q,"%llu %llu %llu %31s %c",&arr,&len,&w,name,&extra) != 4 ||
            njobs == MAX_JOBS || !len || len > 1000000 || !w || w > 10000 || arr > 86400000 ||
            strspn(name,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != strlen(name)) {
            fprintf(stderr,"invalid job at line %d (arrival>=0, base_blocks=1..1000000, weight=1..10000, simple unique name)\n",lineno);
            fclose(f); return 1;
        }
        for (int k=0;k<njobs;k++) if (!strcmp(jobs[k].name,name)) {
            fprintf(stderr,"duplicate job name at line %d\n",lineno); fclose(f); return 1;
        }
        jobs[njobs] = (struct job){.arrival_ns=arr*1000000ull,.base_blocks=len,
                         .total_records=len*JOB_BASE_BLOCK_RECORDS,.weight=w};
        snprintf(jobs[njobs].name,sizeof jobs[njobs].name,"%s",name); njobs++;
    }
    if (ferror(f)) { perror("jobfile read"); fclose(f); return 1; }
    fclose(f);
    if (!njobs) { fprintf(stderr,"job file must contain at least one job\n"); return 1; }
    qsort(jobs,(size_t)njobs,sizeof *jobs,cmp_arrival);
    quantum_ns=(uint64_t)quantum_ms*1000000ull;
    nrec=INPUT_BYTES/sizeof(uint32_t);
    block_recs=(size_t)block_kb*1024/sizeof(uint32_t);
    for (int i=0;i<njobs;i++) {
        uint64_t full=jobs[i].total_records/nrec, tail=jobs[i].total_records%nrec;
        jobs[i].length=full*((nrec+block_recs-1)/block_recs)+(tail+block_recs-1)/block_recs;
    }
    data = malloc(nrec * sizeof *data);
    if (!data) {
        perror("malloc");
        return 1;
    }
    fill_records(data, nrec);
    printf("pid=%d policy=%s quantum_ms=%d block_KB=%d workers=%d jobs=%d data_MB=%.0f\n", (int)getpid(),
           policy == 0 ? "fifo" : policy == 1 ? "rr" : "wrr", quantum_ms, block_kb, workers, njobs, mb);
    fflush(stdout);

    char tracepath[4096];
    if (snprintf(tracepath,sizeof tracepath,"%s.turns.csv",csv) >= (int)sizeof tracepath) {
        fprintf(stderr,"CSV path too long\n"); free(data); return 1;
    }
    turnlog=fopen(tracepath,"w");
    if (!turnlog) { perror(tracepath); free(data); return 1; }
    fprintf(turnlog,"job,turn_start_ms,turn_end_ms,turn_cpu_ms,cumulative_cpu_ms,done_records,weight\n");
    struct warg wa[16];
    pthread_t th[16], rel;
    t0 = now_ns();
    for (int i = 0; i < workers; i++) {
        wa[i] = (struct warg){ .id = i };
        int rc=pthread_create(&th[i],NULL,worker,&wa[i]);
        if (rc) { fprintf(stderr,"pthread_create: %s\n",strerror(rc)); return 1; }
    }
    int rc=pthread_create(&rel,NULL,releaser,NULL);
    if (rc) { fprintf(stderr,"releaser pthread_create: %s\n",strerror(rc)); return 1; }
    pthread_join(rel, NULL);
    for (int i = 0; i < workers; i++) {
        pthread_join(th[i], NULL);
    }

    uint64_t wall_end=now_ns(); /* 직렬 검증/CSV 출력은 실행 wall time에서 제외 */
    fclose(turnlog);
    int ok = finished_jobs == njobs;
    for (int i=0;i<njobs;i++) {
        struct job *j=&jobs[i];
        if (!j->finished || j->done_records!=j->total_records || j->done_blocks!=j->length ||
            !j->first_start_ns || !j->completion_ns) {
            fprintf(stderr,"%s: INCOMPLETE (TODO not implemented or scheduler error)\n",j->name); ok=0;
        }
    }
    if (!ok) { fprintf(stderr,"performance metrics suppressed until all jobs complete\n"); free(data); return 2; }
    uint64_t full_sum,full_count;
    reference_aggregate(data,nrec,digit,&full_sum,&full_count);
    for (int i=0;i<njobs;i++) {
        struct job *j=&jobs[i]; uint64_t tail_sum,tail_count;
        reference_aggregate(data,(size_t)(j->total_records%nrec),digit,&tail_sum,&tail_count);
        uint64_t ref_sum=(j->total_records/nrec)*full_sum+tail_sum;
        uint64_t ref_count=(j->total_records/nrec)*full_count+tail_count;
        if (j->sum!=ref_sum || j->kept!=ref_count) {
            fprintf(stderr,"%s: MISMATCH kept=%llu/%llu sum=%llu/%llu\n",j->name,
                (unsigned long long)j->kept,(unsigned long long)ref_count,(unsigned long long)j->sum,(unsigned long long)ref_sum);
            ok=0;
        }
    }
    if (!ok) { free(data); return 2; }
    printf("validation: all jobs COMPLETE and aggregates OK\n");

    /* 출력 (제공) */
    uint64_t total_cpu = 0, overhead = 0, work = 0;
    for (int i = 0; i < workers; i++) {
        overhead += wa[i].overhead_ns;
        work += wa[i].work_ns;
    }
    printf("%-10s %8s %8s %6s %10s %10s %10s %8s %8s %10s\n", "job", "arr_ms", "len", "weight", "start_ms", "done_ms", "resp_ms", "cpu_ms", "preempt", "maxturn_ms");
    FILE *c = fopen(csv,"w");
    if (!c) { perror(csv); free(data); return 1; }
    if (c) {
        fprintf(c, "job,arrival_ms,length_blocks,weight,first_start_ms,completion_ms,response_ms,cpu_ms,preemptions,max_turn_ms,total_records,kept,sum\n");
    }
    for (int i = 0; i < njobs; i++) {
        struct job *j = &jobs[i];
        double arr = j->arrival_ns / 1e6, st = j->first_start_ns ? (j->first_start_ns - t0) / 1e6 : 0, dn = j->completion_ns ? (j->completion_ns - t0) / 1e6 : 0;
        printf("%-10s %8.0f %8llu %6llu %10.1f %10.1f %10.1f %8.1f %8llu %10.2f\n", j->name, arr, (unsigned long long)j->length,
               (unsigned long long)j->weight, st, dn, dn - arr, j->cpu_ns / 1e6, (unsigned long long)j->preemptions, j->max_turn_ns / 1e6);
        if (c) {
            fprintf(c, "%s,%.0f,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%llu,%.2f,%llu,%llu,%llu\n", j->name, arr, (unsigned long long)j->length, (unsigned long long)j->weight,
                    st, dn, dn - arr, j->cpu_ns / 1e6, (unsigned long long)j->preemptions,j->max_turn_ns/1e6,
                    (unsigned long long)j->total_records,(unsigned long long)j->kept,(unsigned long long)j->sum);
        }
        total_cpu += j->cpu_ns;
    }
    if (c) {
        fclose(c);
    }
    printf("total_cpu_ms=%.1f  sched_overhead_ms=%.2f (%.2f%% of work)  wall_ms=%.1f\n", total_cpu / 1e6, overhead / 1e6,
           100.0 * overhead / (work ? work : 1), (wall_end - t0) / 1e6);
    free(data);
    return 0;
}
