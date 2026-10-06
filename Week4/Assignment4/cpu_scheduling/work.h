/* Assignment 4 공통 헤더 (제공 코드). dengine(Level 1)과 usched(Level 3)이 함께 쓴다.
 * 제공: 데이터 생성, 변환 함수, 조건 검사 함수, 시간 측정 helper, 직렬 검증 함수
 * 구현: work_block()  (TODO 표시) */
#ifndef WORK_H
#define WORK_H
#define _GNU_SOURCE
#include <stdint.h>
#include <limits.h>
#define INPUT_BYTES ((size_t)64 * 1024 * 1024)
#define FILTER_RESIDUE 3
#define JOB_BASE_BLOCK_BYTES ((uint64_t)256 * 1024)
#define JOB_BASE_BLOCK_RECORDS (JOB_BASE_BLOCK_BYTES / sizeof(uint32_t))
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/syscall.h>

/* wall clock (ns) */
static inline uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* 호출한 thread가 소비한 CPU 시간 (ns). Level 3의 quantum 계산에 쓴다. */
static inline uint64_t thread_cpu_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* 입력 데이터를 고정된 pseudo-random pattern으로 채운다. 모든 구성에서 같은 결과가 나와야 한다. */
static inline void fill_records(uint32_t *a, size_t n) {
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < n; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        a[i] = (uint32_t)(x >> 32);
    }
}

/* 1단계 변환: 고정된 산술 연산 */
static inline uint32_t transform(uint32_t v) {
    return v * 2654435761u + 12345u;
}

/* 2단계 조건 검사: 값을 7로 나눈 나머지가 d(과제에서는 3으로 고정)인 record만 통과 */
static inline int keep(uint32_t v, int d) {
    return (int)(v % 7u) == d % 7;
}

/* TODO (Level 1): block 하나에 세 단계를 모두 적용한다.
 *   in[0..n)  입력 record
 *   out       변환 결과를 저장할 buffer (NULL이면 저장하지 않음)
 *   d         조건 검사 기준값 (과제에서는 3)
 *   *sum      통과한 변환값의 합을 더해서 돌려준다
 *   *count    통과한 record 수를 더해서 돌려준다
 * Level 3의 usched가 이 함수를 그대로 재사용하므로 전역 상태에 의존하지 않게 작성한다. */
__attribute__((noinline)) static void work_block(const uint32_t *in, uint32_t *out, size_t n, int d, uint64_t *sum, uint64_t *count) {
    (void)in; (void)out; (void)n; (void)d; (void)sum; (void)count;
    /* TODO */
}

/* 직렬 검증 함수 (제공). 자기 구현의 seen/kept/sum이 이 값과 같은지 확인한다. */
static inline void reference_aggregate(const uint32_t *in, size_t n, int d, uint64_t *sum, uint64_t *count) {
    uint64_t s = 0, c = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = transform(in[i]);
        if (keep(v, d)) {
            s += v;
            c++;
        }
    }
    *sum = s;
    *count = c;
}

/* sched_getaffinity()로 센 실제 사용 가능 CPU 수. cpuset 안에서는 nproc과 다르다. */
static inline int allowed_cpus(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set)) {
        return (int)sysconf(_SC_NPROCESSORS_ONLN);
    }
    return CPU_COUNT(&set);
}

static inline void set_thread_name(const char *prefix, int idx) {
    char name[16];
    snprintf(name, sizeof name, "%.11s-%d", prefix, idx);
    pthread_setname_np(pthread_self(), name);
}
#endif
