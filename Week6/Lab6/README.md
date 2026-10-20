# Week 6 실습: race, spinning lock과 sleeping lock, rwlock과 RCU, condition variable 버그

강의에서 다룬 공유 데이터의 race, spinlock과 mutex가 기다리는 방식의 차이, 읽기가 대부분인 데이터에서 rwlock과 RCU의 차이, condition variable과 mutex 사용 오류를 작은 프로그램으로 직접 재현한다. 자세한 절차와 확인 질문은 `Lab6_instructions.docx`에 있다.

## 준비
```
bash check_env.sh       # 환경 점검. WARN이 있으면 Lab 문서의 표를 보고 조치한다
make                    # bin/counter, bin/counter_tsan, bin/lockbench, bin/cfgread, bin/cvbug
```
bpftrace와 perf는 Week 5에서 설치한 것을 그대로 쓴다. 실습 폴더는 `~/course-labs/week6_lab`에 두고 사용한다.

## 실습 명령은 tools/lab.sh 하나로
```
bash tools/lab.sh                           # 단계 목록
bash tools/lab.sh 1       # 실습 1-1
bash tools/lab.sh 1tsan   # 실습 1-2
bash tools/lab.sh 2a      # 실습 2-1
bash tools/lab.sh 2b      # 실습 2-2
bash tools/lab.sh 2c      # 실습 2-3
bash tools/lab.sh 2d      # 실습 2-4
bash tools/lab.sh 3ifwait # 실습 3-1
bash tools/lab.sh 3iolock # 실습 3-2
bash tools/lab.sh 3hang   # 실습 3-3
bash tools/lab.sh 3futex  # 실습 3-4
```
2a, 2b, 3hang, 3futex는 시작할 때 sudo 암호를 한 번 묻고 그 단계를 root로 실행한다(perf trace, gdb, bpftrace를 쓰기 때문이다). 명령은 한 줄씩 입력한다.

## 실습 구성
| 실습 | 내용 | 쓰는 도구 |
| --- | --- | --- |
| 1 | counter++ race와 atomic의 비용, 일반 빌드와 TSan 빌드의 분리 | counter, counter_tsan |
| 2 | spinlock, MCS lock, mutex, adaptive mutex: 경쟁이 없을 때, 있을 때, thread가 CPU보다 많을 때. 읽기가 대부분인 설정값의 rwlock과 RCU, grace period | lockbench, cfgread, perf trace (안 되면 strace) |
| 3 | condvar와 mutex 사용 오류 세 가지(if 대기, lock 안의 I/O, unlock 누락)와 hang 추적 | cvbug, gdb, /proc/PID/task/*/wchan, tools/futex.bt |

## 프로그램
```
./bin/counter   [-m plain|atomic|mutex|local] [-t threads] [-n increments]
./bin/lockbench -m spin|mcs|mutex|adaptive [-t threads] [-n ops] [-c cs_loops] [-w work_loops] [-T max_seconds]
./bin/cfgread   -m rwlock|rcu|nogp [-r readers] [-d seconds] [-u update_us]
./bin/cvbug     -m ifwait|iolock|nounlock|fixed [-n items] [-s slow_ms]
sudo bpftrace tools/futex.bt -c '<command>'
```

## 주의
- 시간은 일반 빌드로만 잰다. TSan 빌드(bin/counter_tsan)는 계측 때문에 훨씬 느리다.
- TSan 프로그램이 `unexpected memory mapping`으로 멈추면 `sudo sysctl vm.mmap_rnd_bits=28`을 실행한다.
- `lockbench -m mcs`를 thread가 CPU보다 많게 실행하면 매우 느려진다. 그래서 `-T`(기본 5초)가 지나면 그때까지 한 만큼으로 결과를 낸다.
- `cfgread -m nogp`는 grace period 없이 free하는 일부러 틀린 버전이다(해제된 memory를 읽는다).
- `cvbug -m nounlock`은 일부러 멈추는 프로그램이다. 직접 실행했다면 Ctrl-C로 끝낸다.
- 실습 출력은 logs/에 남는다. cvbug가 쓰는 log 파일만 /tmp에 쓰고 끝나면 지운다.