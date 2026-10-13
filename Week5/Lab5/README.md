# Week 5 실습: scheduling class별 알고리즘을 kernel 기록으로 확인하기

강의 Part 2에서 본 세 class(deadline, rt, fair)를 하나씩 직접 돌려 보고, 각 class가 다음 task를 고르는 규칙을 perf와 bpftrace로 확인한다. 자세한 절차와 확인 질문은 `Lab5_instructions.docx`에 있다.

## 준비
```
bash tools/install_bpftrace.sh     # 실습 0: apt로 설치하고 실제로 돌려 본다. 안 되면 v0.20.4 정적 실행 파일을 받는다
bash check_env.sh                  # 환경 점검. WARN이 있으면 Lab 문서의 표를 보고 조치한다
make                               # bin/mtwork, bin/pi_demo
```
실습 폴더는 `~/course-labs/week5_lab`에 두고 사용한다.

## 실습 명령은 tools/lab.sh 하나로
프로그램 두세 개를 동시에 돌려야 하는 단계도 `bash tools/lab.sh <단계>` 한 줄로 실행한다. script가 부하를 background로 띄우고, 측정과 관측을 알맞은 시점에 실행한 뒤 결과를 모아 보여 준다.
```
bash tools/lab.sh                  # 단계 목록
bash tools/lab.sh 0                # 실습 0
bash tools/lab.sh 1a | 1b | 1c     # 실습 1, 한 번에 하나씩
bash tools/lab.sh 2fifo | 2rr | 2pi    # 실습 2, 한 번에 하나씩
bash tools/lab.sh 3 base busy,cpu=0    # 추가 실습 한 조건 (slice 단계는 bash tools/lab.sh 3slice)
```

## 실습 구성
| 실습 | 내용 | 쓰는 도구 |
| --- | --- | --- |
| 0 | bpftrace 설치(0.20 이상), tracepoint 목록 확인, 첫 one-liner | install_bpftrace.sh, check_env.sh |
| 1 | SCHED_DEADLINE: runtime 안에서 끝나는 job, runtime을 넘는 job(CBS throttle), admission control | mtwork `sched=dl:R/P`, tools/perf_sched.sh |
| 2 | FIFO와 Round Robin, RT throttling, priority inversion과 priority inheritance | mtwork `sched=fifo:P` `sched=rr:P`, pi_demo, bt/piwatch.bt, tools/perf_sched.sh |
| 3 | EEVDF: indexer에 nice, base slice, SCHED_BATCH, SCHED_IDLE을 적용하며 renderer가 기다린 시간 비교 | tools/eevdf_run.sh, bt/runq.bt, bt/preempt.bt, tools/slice.sh |

## mtwork (Week 5판) 사용법
```
./bin/mtwork [-d 초] [-n 이름] <thread spec> [<thread spec> ...]
  thread spec = 일의 종류[,cpu=N][,sched=POLICY][,nice=N]
    busy                CPU를 계속 쓰는 thread
    periodic:P:W        P us마다 깨어나 CPU 시간으로 W us만큼 일하는 thread
    sched=other|batch|idle|fifo:P|rr:P|dl:R/P   (dl은 period P us마다 runtime R us)
```
- periodic job의 크기는 thread가 실제로 CPU에서 쓴 시간으로 잰다. preempt되거나 throttle되어 기다린 시간은 포함하지 않는다. Week 4판은 wall time으로 쟀으므로 숫자를 비교할 때 주의한다.
- 출력 열: `cpu_ms`(쓴 CPU 시간), `wait_ms`(runnable인데 CPU를 받지 못한 시간의 합), `late_p99u`(job이 늦게 시작한 정도), `resp_p99u` / `resp_maxu`(job이 시작돼야 할 시각부터 끝날 때까지 걸린 시간), `miss`(자기 period 안에 끝나지 못한 job 수)
- 모든 thread는 같은 시각에 측정을 시작하고 같은 시각에 끝낸다. 측정하는 동안 CPU를 한 번도 받지 못한 thread는 cpu_ms가 0으로 나온다.

## 주의
- Ubuntu 22.04의 apt 패키지(bpftrace 0.14)는 kernel BTF를 읽지 못하면 tracepoint 인자를 해석하지 못한다. install_bpftrace.sh가 이를 확인하고 공식 release의 정적 실행 파일(v0.20.4)을 /usr/local/bin에 설치한다(GitHub 접속 필요).
- Lab 문서의 명령 끝에 붙은 # A, # B, # C는 lab.sh 없이 손으로 실행할 때 쓰는 서로 다른 터미널을 뜻한다. 보통 B(부하)를 먼저 시작하고, B가 도는 동안 A, A가 도는 동안 C를 실행한다.
- RT, DEADLINE, bpftrace, perf를 쓰는 명령은 sudo가 필요하다.
- 실습은 vCPU 2개(.wslconfig의 processors=2) 기준이다. CPU가 더 많으면 lab.sh가 실습 1의 busy thread 수와 admission control 요청 수를 CPU 수에 맞춰 늘린다.
- 실습 2의 FIFO busy thread는 CPU 0을 차지한다. CPU 0에서 돌던 셸이 몇 초 동안 느려질 수 있는데, RT throttling이 일반 task에게 1초에 약 50ms를 남겨 주므로 기다리면 돌아온다.
- perf data는 /tmp에 저장한다.
- 추가 실습의 slice 단계를 마치면 반드시 `sudo bash tools/slice.sh restore`로 원래 값을 되돌린다.

## 검증 상태
- sandbox(kernel 6.18, vCPU 2)에서 모든 프로그램, bpftrace 스크립트(bpftrace 0.20.2와 0.20.4), perf_sched.sh, eevdf_run.sh, lab.sh의 모든 단계를 실행해 확인했다. 6.18에는 fair server와 task별 slice가 있어 WSL2(6.6)와 수치가 다를 수 있다.
- base_slice_ns 변경(실습 3의 slice 단계)은 sandbox에서 debugfs 쓰기가 막혀 있어 실행하지 못했다. WSL2에서 확인이 필요하다.
- WSL2에서 재현할 수 없는 것: EAS, schedutil, uclamp, cpu_capacity(x86 vCPU), fair server와 task별 slice(6.12 이후 기능)
