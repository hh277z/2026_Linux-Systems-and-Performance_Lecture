/* Level 1 스켈레톤.
 * 제공: 고정 입력, persistent worker pool, 측정/검증, CLI와 출력.
 * TODO: work.h의 work_block(), 아래 run_block(), stage/block 처리, progress 출력.
 * worker는 반복 사이에도 살아 있어 Level 2에서 TID를 지속적으로 관측할 수 있다. */
#include "work.h"
#include <stdatomic.h>
#include <getopt.h>

struct stats { uint64_t seen, kept, sum; };
struct stats_padded { uint64_t seen, kept, sum; char pad[64 - 3 * sizeof(uint64_t)]; } __attribute__((aligned(64)));
static uint32_t *in, *out;
static unsigned char *passed; /* stage의 filter 결과. aggregate 단계에서 다시 읽는다. */
static size_t nrec, block_recs;
static int nthreads, mode_block = 1, padded = 1, per_block = 1;
static const int digit = FILTER_RESIDUE;
static struct stats *packed_stats;
static struct stats_padded *padded_stats;
static atomic_size_t next_block;
static atomic_uint_fast64_t blocks_done; /* 반복 전체의 누적값: 반복 시작에 초기화하지 않는다. */
static atomic_int pool_stop, progress_stop;
static uint64_t progress_epoch;
static pthread_barrier_t ready_barrier, start_barrier, finish_barrier, stage_barrier;
#define SEEN(t) (padded ? padded_stats[t].seen : packed_stats[t].seen)
#define KEPT(t) (padded ? padded_stats[t].kept : packed_stats[t].kept)
#define SUM(t)  (padded ? padded_stats[t].sum : packed_stats[t].sum)

static inline void stat_add(int t, uint64_t seen, uint64_t kept, uint64_t sum) {
    if (padded) {
        padded_stats[t].seen += seen; padded_stats[t].kept += kept; padded_stats[t].sum += sum;
    } else {
        packed_stats[t].seen += seen; packed_stats[t].kept += kept; packed_stats[t].sum += sum;
    }
}
struct targ { int t; long tid; double cpu_ms; long vol, invol; };

/* TODO: b번째 block의 off=b*block_recs, n=min(block_recs,nrec-off).
 * per_block: work_block()에 local sum/count를 전달한 뒤 stat_add(t,n,count,sum) 한 번.
 * record: 각 record에 transform/keep 후 stat_add()를 호출한다.
 * 마지막에 atomic_fetch_add(&blocks_done,1). 두 경로의 집계 결과는 동일해야 한다. */
static void run_block(int t, size_t b) {
    (void)t; (void)b;
    /* TODO */
}
static void *worker(void *p) {
    struct targ *a = p;
    int t = a->t;
    set_thread_name("worker", t);
    a->tid = syscall(SYS_gettid);
    pthread_barrier_wait(&ready_barrier);
    for (;;) {
        struct rusage u0, u1;
        getrusage(RUSAGE_THREAD, &u0);
        pthread_barrier_wait(&start_barrier);
        if (atomic_load(&pool_stop)) break;
        if (mode_block) {
            /* TODO: atomic_fetch_add(&next_block,1)로 block을 배정받아 run_block().
             * 유효 block 수는 (nrec+block_recs-1)/block_recs. 범위를 넘으면 종료. */
        } else {
            /* TODO: slice [nrec*t/nthreads, nrec*(t+1)/nthreads).
             * 1) transform(in[i])를 out[i]에 저장
             * 2) pthread_barrier_wait(&stage_barrier)
             * 3) passed[i]=keep(out[i],digit)로 filter 결과만 저장 (아직 집계하지 않음)
             * 4) pthread_barrier_wait(&stage_barrier)
             * 5) out/passed를 다시 읽어 aggregate. -u record는 record마다 stat_add,
             *    -u block은 slice를 block_recs 단위로 나눠 local 누적 후 한 번 갱신.
             * 처리한 slice의 논리 block 수를 blocks_done에 더한다.
             * stage_barrier는 worker 전용이다. main용 barrier를 대체 사용하지 않는다. */
        }
        getrusage(RUSAGE_THREAD, &u1);
        a->cpu_ms = (u1.ru_utime.tv_sec-u0.ru_utime.tv_sec)*1e3 + (u1.ru_utime.tv_usec-u0.ru_utime.tv_usec)/1e3 +
                    (u1.ru_stime.tv_sec-u0.ru_stime.tv_sec)*1e3 + (u1.ru_stime.tv_usec-u0.ru_stime.tv_usec)/1e3;
        a->vol = u1.ru_nvcsw-u0.ru_nvcsw;
        a->invol = u1.ru_nivcsw-u0.ru_nivcsw;
        pthread_barrier_wait(&finish_barrier);
    }
    return NULL;
}
/* TODO (-P): progress_stop이 0인 동안 약 1초마다 sleep하고 blocks_done 증가량 출력.
 * 실제 interval 길이로 blocks/s를 계산한다. 출력 형식:
 * progress mono_ns=<now_ns()> elapsed_s=<since progress_epoch> interval_s=<seconds> blocks/s=<rate>
 * timestamp는 cpumgr의 monotonic timestamp와 정렬에 사용한다.
 * 종료 시 마지막 partial interval도 출력한다. busy waiting 금지. */
static void *progress_thread(void *p) {
    (void)p;
    /* TODO */
    return NULL;
}
static void usage(const char *name) {
    fprintf(stderr,"usage: %s [-n 64] [-t threads] [-m stage|block] [-b block_KB] [-s packed|padded] [-u record|block] [-r repeats] [-P]\n",name);
}
int main(int argc, char **argv) {
    int repeats=1, block_kb=256, progress=0, opt;
    nthreads=allowed_cpus();
    while ((opt=getopt(argc,argv,"n:t:m:b:s:u:r:Ph"))!=-1) {
        switch(opt) {
        case 'n': if(strcmp(optarg,"64")) { fprintf(stderr,"input is fixed to 64 MB\n"); return 1; } break;
        case 't': nthreads=atoi(optarg); break;
        case 'm': if(strcmp(optarg,"stage")&&strcmp(optarg,"block")) goto bad; mode_block=!strcmp(optarg,"block"); break;
        case 'b': block_kb=atoi(optarg); break;
        case 's': if(strcmp(optarg,"packed")&&strcmp(optarg,"padded")) goto bad; padded=!strcmp(optarg,"padded"); break;
        case 'u': if(strcmp(optarg,"record")&&strcmp(optarg,"block")) goto bad; per_block=!strcmp(optarg,"block"); break;
        case 'r': repeats=atoi(optarg); break;
        case 'P': progress=1; break;
        default: goto bad;
        }
    }
    if(optind!=argc || nthreads<1 || nthreads>64 || repeats<1 || block_kb<4 || block_kb>65536) {
 bad: usage(argv[0]); return 1;
    }
    nrec=INPUT_BYTES/sizeof(uint32_t); block_recs=(size_t)block_kb*1024/sizeof(uint32_t);
    in=malloc(INPUT_BYTES); out=malloc(INPUT_BYTES); passed=malloc(nrec);
    packed_stats=calloc((size_t)nthreads,sizeof *packed_stats);
    padded_stats=aligned_alloc(64,(size_t)nthreads*sizeof *padded_stats);
    if(!in||!out||!passed||!packed_stats||!padded_stats) { perror("alloc"); return 1; }
    fill_records(in,nrec);
    uint64_t ref_sum,ref_count;
    reference_aggregate(in,nrec,digit,&ref_sum,&ref_count); /* 측정 전에 기준값 계산 */
    printf("pid=%d nproc=%ld allowed_cpus=%d threads=%d mode=%s block_KB=%d stats=%s update=%s records=%zu residue=%d\n",
      (int)getpid(),sysconf(_SC_NPROCESSORS_ONLN),allowed_cpus(),nthreads,mode_block?"block":"stage",block_kb,
      padded?"padded":"packed",per_block?"block":"record",nrec,digit);
    pthread_barrier_init(&ready_barrier,NULL,(unsigned)nthreads+1);
    pthread_barrier_init(&start_barrier,NULL,(unsigned)nthreads+1);
    pthread_barrier_init(&finish_barrier,NULL,(unsigned)nthreads+1);
    pthread_barrier_init(&stage_barrier,NULL,(unsigned)nthreads);
    struct targ args[64],best_args[64]; pthread_t th[64],pt;
    for(int t=0;t<nthreads;t++) {
        args[t]=(struct targ){.t=t};
        int rc=pthread_create(&th[t],NULL,worker,&args[t]);
        if(rc) { fprintf(stderr,"pthread_create: %s\n",strerror(rc)); return 1; }
    }
    pthread_barrier_wait(&ready_barrier);
    printf("TIDS="); for(int t=0;t<nthreads;t++) printf("%s%ld",t?",":"",args[t].tid); printf("\n"); fflush(stdout);
    progress_epoch=now_ns();
    if(progress) {
        int rc=pthread_create(&pt,NULL,progress_thread,NULL);
        if(rc) { fprintf(stderr,"progress pthread_create: %s\n",strerror(rc)); return 1; }
    }
    double best_ms=1e18; int best_rep=0,completed=0,ok=1;
    uint64_t seen=0,kept=0,sum=0;
    for(int rep=0;rep<repeats;rep++) {
        memset(packed_stats,0,(size_t)nthreads*sizeof *packed_stats);
        memset(padded_stats,0,(size_t)nthreads*sizeof *padded_stats);
        atomic_store(&next_block,0);
        uint64_t begin=now_ns(); /* 시작 시각 기록 후 worker 해제 */
        pthread_barrier_wait(&start_barrier);
        pthread_barrier_wait(&finish_barrier);
        double ms=(now_ns()-begin)/1e6;
        if(ms<best_ms) { best_ms=ms; best_rep=rep+1; memcpy(best_args,args,(size_t)nthreads*sizeof *args); }
        seen=kept=sum=0;
        for(int t=0;t<nthreads;t++) { seen+=SEEN(t); kept+=KEPT(t); sum+=SUM(t); }
        completed++;
        if(seen!=nrec||kept!=ref_count||sum!=ref_sum) { ok=0; fprintf(stderr,"repeat %d: MISMATCH (TODO 미완료 또는 집계 오류)\n",rep+1); break; }
    }
    atomic_store(&progress_stop,1);
    if(progress) pthread_join(pt,NULL);
    printf("result: seen=%llu kept=%llu sum=%llu (reference kept=%llu sum=%llu) %s\n",
      (unsigned long long)seen,(unsigned long long)kept,(unsigned long long)sum,(unsigned long long)ref_count,(unsigned long long)ref_sum,ok?"OK":"MISMATCH");
    if(ok) {
        printf("elapsed_ms(best of %d, repeat %d)=%.3f input_GB/s=%.3f\n",completed,best_rep,best_ms,INPUT_BYTES/(best_ms/1e3)/1e9);
        printf("thr cpu_ms vol invol (all from best repeat)\n");
        for(int t=0;t<nthreads;t++) printf("%d %.3f %ld %ld\n",t,best_args[t].cpu_ms,best_args[t].vol,best_args[t].invol);
    } else printf("performance metrics suppressed until correctness passes\n");
    fflush(stdout);
    atomic_store(&pool_stop,1); pthread_barrier_wait(&start_barrier);
    for(int t=0;t<nthreads;t++) pthread_join(th[t],NULL);
    pthread_barrier_destroy(&ready_barrier); pthread_barrier_destroy(&start_barrier);
    pthread_barrier_destroy(&finish_barrier); pthread_barrier_destroy(&stage_barrier);
    free(in);free(out);free(passed);free(packed_stats);free(padded_stats);
    return ok?0:2;
}
