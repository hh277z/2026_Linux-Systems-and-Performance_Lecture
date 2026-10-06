# Assignment 4 — CPU Sharing Lab

학생용 스켈레톤이다. **TODO는 구현하지 않은 상태**이며, TODO를 완성해야 검증과 실험을 수행할 수 있다. 스켈레톤과 구조체는 필요한 경우 수정할 수 있다. 수정 이유와 검증 방법을 보고서에 남긴다.

## 환경과 빌드

- Linux / WSL2 kernel 6.6, systemd=true, cgroup v2 cpu/cpuset controller.
- 과제 실험은 vCPU 2개(processors=2)로 수행한다.
- C compiler, make, pthread가 필요하다. Level 2는 cgroup에 쓸 권한이 필요하다.

```bash
make
uname -r
lscpu
```

파일 이름은 `dengine.c`, `cpumgr.c`, `usched.c`, `work.h`다. 첨부 파일에 붙은 `(1)`은 ZIP 안의 파일명에서는 제거했다. 기본 빌드 옵션은 `-O2 -g -fno-omit-frame-pointer -Wall -Wextra`다. 모든 비교에서 동일한 옵션을 사용한다. TODO 미구현 상태에서 unused function/variable 경고는 예상된다.

## 고정값

- 입력: 64MB의 32-bit record (`INPUT_BYTES`, 코드의 MB는 1024×1024 bytes).
- filter: 변환값의 modulo 7이 3인 record만 통과.
- 짧은 job: **256KB 기준 4 blocks = 1MB**.
- `-n 64`는 기존 명령 호환성을 위해 지원한다. 다른 입력 크기는 거부한다. `-D`는 제거했다.

## Level 1 — dengine

구현할 TODO:

1. `work.h`: 전역 상태에 의존하지 않는 `work_block()`. `out==NULL`도 지원하고 기존 `*sum`, `*count`에 결과를 더한다.
2. `run_block()`: 마지막 partial block 처리, record/block counter 갱신, 누적 blocks_done 갱신.
3. `worker()`: atomic block 배정 또는 stage 처리.
4. `progress_thread()`: sleep을 사용하는 1초 sampling, timestamp/실제 interval/blocks per second 출력, 종료 시 마지막 partial interval 출력.

stage 순서는 **transform → worker barrier → filter → worker barrier → aggregate**다. `passed[]`에 filter 결과를 저장하고 aggregate loop에서 out/passed를 다시 읽는다. record mode는 record마다 통계 갱신, block mode는 slice를 block 크기로 나누어 local 누적 후 갱신한다.

제공된 worker pool은 반복 사이에도 TID를 유지한다. main은 ready barrier 이후 TID를 출력하고, 각 반복의 시작 시각을 기록한 다음 start barrier로 worker를 해제한다. worker는 처리와 통계 기록 후 finish barrier에서 main을 기다린다. main이 초기화하는 input/output/statistics를 barrier 밖에서 접근하지 않는다. stage_barrier는 worker 전용이다.

```bash
./dengine -n 64 -t 2 -m block -b 256 -s padded -u block -r 3
./dengine -n 64 -t 2 -m stage -b 256 -r 3
```

각 반복의 seen/kept/sum을 직렬 기준값과 비교한다. 기본 입력의 기준값:

```text
seen=16777216 kept=2398008 sum=5146055400178138
```

TODO 미완료 또는 잘못된 집계는 `MISMATCH`와 exit code 2를 반환하고 성능 수치를 숨긴다. 성공한 경우 best elapsed와 **같은 반복**의 CPU time/context switch를 출력한다. input_GB/s = 입력 bytes / elapsed seconds / 10^9다. 총 memory traffic의 추정값이 아니다. elapsed와 context switch에는 측정 구간의 제어 barrier 비용이 일부 포함된다.

## Level 2 — cpumgr

구현할 TODO:

- controller 활성화 및 PID별 job/background group 생성과 초기 설정 적용.
- `spawn_in_group()`의 child 경로: 자신의 PID를 cgroup.procs에 쓴 후 exec 또는 두 thread의 burner 실행.
- `schedstat_delta()`: TID별 exec/run_delay 증가량.
- 주기 sampling, --at 이벤트, child 종료 상태 전달, teardown.

`fork()`와 child의 signal handler 초기화는 제공한다. background burner는 exec하지 않으므로 부모의 handler를 그대로 쓰면 종료되지 않을 수 있다. 제공된 `terminate_child()`로 종료시키고 wait한 다음 cgroup을 제거한다. cgroup 설정/읽기/child 실행이 실패하면 성공으로 보고하지 않는다.

Job과 background는 `/sys/fs/cgroup/cpumgr/<manager-pid>`와 `bg-<manager-pid>`이며 sibling group이다. 초기화한 부모 controller 상태를 다른 실행에 영향을 주도록 무조건 되돌리지 않는다. base directory가 다른 실행에서 사용 중이면 남겨도 된다.

```bash
mkdir -p logs
sudo ./cpumgr --bg 100 --weight 100 \
  --at 10:weight=20 --at 20:weight=400 \
  -- ./dengine -n 64 -t 2 -m block -b 256 -r 1000 -P \
  2>&1 | tee logs/weight.txt

sudo ./cpumgr --max 50000 100000 \
  --at 10:max=20000,100000 --at 20:max=max,100000 \
  -- ./dengine -n 64 -t 2 -m block -b 256 -r 1000 -P \
  2>&1 | tee logs/quota.txt

sudo ./cpumgr --cpuset 0 \
  -- ./dengine -n 64 -m block -b 256 -r 100 -P \
  2>&1 | tee logs/cpuset-default.txt
sudo ./cpumgr --cpuset 0 \
  -- ./dengine -n 64 -t 4 -m block -b 256 -r 100 -P \
  2>&1 | tee logs/cpuset-four.txt
```

반복 횟수는 예시이며 실제 실행 시간을 확인해야 한다.
실행이 20초 이후까지 계속되도록 `-r`을 조정하고 사용한 값을 보고한다. progress는 stderr이므로 `2>&1`로 저장한다.

cpumgr와 progress의 `mono_ns`를 사용해 시간축을 맞춘다. CSV의 usage_ms/throttled_ms/exec_ms/run_delay_ms는 실제 sample 구간의 증가량이며, nr_throttled_delta는 횟수의 증가량이다. nr_throttled를 ms로 환산하지 않는다. --at 변경 전 구간은 이전 설정값으로 sample하고 변경 timestamp와 새 설정은 event에 기록한다. comma가 있는 cpuset 열은 CSV에서 quote한다.

Persistent worker는 반복마다 thread가 종료되어 생기는 큰 누락을 줄인다. 다만 polling으로는 thread 종료 직전 값이 누락될 수 있으므로 마지막 sample과 이 제약을 기록한다. cpu.stat과 /proc/.../schedstat은 대상과 취득 시점도 다르므로 완전 일치를 가정하지 않는다.

## Level 3 — usched

구현할 TODO:

- enqueue/dequeue, job 도착을 담당하는 releaser, condition variable을 사용하는 worker loop.
- FIFO/RR/WRR budget과 block 경계의 requeue.
- job별 sum/kept 보존, CPU time과 overhead 측정, turn log 기록.

Job 파일은 `arrival_ms base_blocks weight name`이다. **base_blocks는 항상 256KB 단위**이며, `-b`는 한 번에 처리할 실행 block의 크기다. 기존의 `length_blocks`를 실행 block 수로 해석하는 방식에서 변경했다. 파일의 두 번째 값은 block 크기 실험에서 바꾸지 않는다.

| -b | short의 입력 base_blocks | 실제 처리 |
|---|---:|---|
| 16KB | 4 | 16KB × 64 blocks |
| 256KB | 4 | 256KB × 4 blocks |
| 4096KB | 4 | 1MB partial block × 1 |

`job_block_span()`은 다음 input offset과 record 수를 제공한다. worker는 이 함수가 반환한 n을 처리하고 done_records에 더한다. data 배열의 끝에서도 partial block을 사용하며, 긴 job은 동일 배열을 처음부터 반복한다. job의 sum/kept는 turn 사이에 보존한다. 출력의 length_blocks는 주어진 -b에서 실제 실행할 block 수다.

```bash
./usched -p rr -q 20 -b 256 -w 1 -j jobs.txt -o logs/rr-20.csv
./usched -p wrr -q 20 -b 256 -w 2 -j jobs.txt -o logs/wrr-two.csv
for b in 16 256 4096; do
  ./usched -p rr -q 20 -b "$b" -w 1 -j jobs.txt -o "logs/rr-block-$b.csv"
done
```

기본 jobs.txt는 긴 job 2개(weight 100/300)와 짧은 job 10개를 담는다. 긴 job이 마지막 short 도착(5500ms)까지 끝나지 않도록 base_blocks를 늘린다. 모든 비교에서는 같은 workload를 사용한다.

완료 후 모든 job의 finished/done_records/done_blocks를 확인하고 직렬 기준값으로 sum/kept를 검증한다. 미완료나 MISMATCH면 exit code 2이며 성능 결과를 출력하지 않는다. `-o`를 생략하면 `usched.csv`를 사용한다. turn log는 `<CSV 경로>.turns.csv`에 생성한다. worker는 제공된 `trace_turn()`을 requeue 전에 호출해야 한다.

측정 정의:

- response time = completion − arrival. 첫 실행 대기 시간과 구분한다.
- Quantum은 `CLOCK_THREAD_CPUTIME_ID` 기반 **전체 turn CPU time**으로 판단한다.
- work CPU time은 work_block 호출 전후로 별도 측정한다. clock/accounting/queue/trace 비용은 bookkeeping에 포함한다.
- overhead % = bookkeeping CPU time / work CPU time × 100. worker별 CPU time을 합산하며 condition wait의 wall time을 더하지 않는다.
- short-job p50/p99: 10개 response time을 정렬하고 nearest-rank `ceil(p×n)`번째 값으로 계산한다. 이 정의에서는 p99가 최댓값이다. 다른 정의를 쓰면 명시한다.
- WRR share: 두 긴 job이 동시에 runnable인 공통 구간을 선택한다. 최종 누적 CPU time 비율은 같은 작업량 때문에 1:1에 가까울 수 있다. turn log를 사용하고 측정 경계를 가로지른 turn을 어떻게 처리했는지 설명한다. 정확한 부분 구간 share가 필요하면 추가 sampling을 구현한다.
- Worker 2개에서 WRR이 1:3을 반드시 보장하지는 않는다. 측정 결과와 이유를 설명한다.
- 기본 short job은 1MB라서 4MB 설정에서도 실제 short의 선점 단위는 1MB다. 4MB block의 overshoot는 long job의 max_turn과 budget을 비교한다.

## 구현 순서와 제출

1. work_block → dengine block → stage → counter 변형 → progress.
2. cpumgr 기본 실행 → weight/max/cpuset → live change → sampling/cleanup.
3. usched FIFO → RR → WRR → worker 2개 → 측정·검증.

```bash
make check
```

TODO 구현 전에는 실패하는 것이 정상이다. 구현 후에는 매 실행 `OK`, 정확한 log/CSV, 정상/Ctrl-C 종료 시 cleanup을 확인한다. Makefile, jobs.txt, README, source, 원본 log, 보고서를 제출한다. 학생은 추가 실험·검증 코드를 작성할 수 있다.
