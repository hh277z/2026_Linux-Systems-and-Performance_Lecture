# Week 11 실습: eMMC 안에서 I/O 시간이 정해지는 방식

강의의 TV eMMC 예제를 바탕으로 만든 장치 모델 emmcsim을 /dev/nbd0로 붙인다. 기본 모델은 2 die × 2 plane, tR 120 µs, tPROG 1 ms, 16 KB page, 400 MB/s 버스를 사용한다. 그 위에서 die 수와 queue depth, fsync와 전원 차단, 큰 write와 UI read의 관계를 직접 잰다. 자세한 절차와 확인 질문은 `Lab11_instructions.docx`에 있다.

I/O는 VFS, ext4, page cache, blk-mq, nbd driver까지 실제 Linux를 지난다. 장치만 모델이다. 실제 eMMC와 다른 점은 driver가 mmc 대신 nbd라는 것과, 장치 시간이 강의 예제값으로 정해진다는 것이다.

## 준비
```
# Windows PowerShell
wsl --update
wsl --version            # 커널 버전: 6.6.x 이상인지
# WSL 안
uname -r                 # 6.6.x-microsoft-standard-WSL2
sudo modprobe nbd && ls /dev/nbd0
sudo apt install -y nbdkit nbdkit-plugin-dev nbd-client fio bpftrace sysstat
bash check_env.sh        # WARN이 있으면 Lab 문서의 표를 보고 조치한다
make                     # bin/emmcsim.so, bin/emmcctl, bin/logger, bin/otawriter
```
실습 폴더는 `~/course-labs/week11_lab`에 두고 사용한다.

## 실습 명령은 tools/lab.sh 하나로
```
bash tools/lab.sh                       # 단계 목록
bash tools/lab.sh 0                     # 준비: nbd 경로 비용
bash tools/lab.sh 1a                  # 실습 1: die 1개
bash tools/lab.sh 1b                  # 실습 1: die 2개
bash tools/lab.sh 1c                  # 실습 1: die 4개
bash tools/lab.sh 1d                  # 실습 1: sequential write
bash tools/lab.sh 2a                  # 실습 2: line마다 fsync
bash tools/lab.sh 2b                  # 실습 2: batch fsync
bash tools/lab.sh 2c                  # 실습 2: powercut 복구
bash tools/lab.sh 3a                  # 실습 3: UI 기준
bash tools/lab.sh 3b                  # 실습 3: suspend=off
bash tools/lab.sh 3c                  # 실습 3: IPS
bash tools/lab.sh 3d                  # 실습 3: IPC
bash tools/lab.sh down                  # 장치 내리기
```
실행하는 주요 명령과 실험 결과 요약을 터미널에 표시하고, 상세 출력과 측정 데이터는 `logs/`에 저장한다. 각 `==` 실험 구간은 빈 줄로 구분한다. 실행 명령과 핵심 결과를 터미널에 표시하며, 성능 측정 단계의 die 통계는 같은 형식으로 표시한다. 전원 차단 실험은 복구 결과를 확인하는 단계이므로 die 통계를 출력하지 않는다. 장치 연결과 파일 시스템 조작은 root 권한으로 수행한다. 각 단계는 emmcsim을 새로 띄우므로 앞 단계의 data는 남지 않는다.

## 실습 구성
| 실습 | 내용 | 쓰는 도구 |
| --- | --- | --- |
| 1 | die 수와 queue depth가 random read와 sequential write의 속도를 어떻게 정하는가 (강의 32, 42, 45쪽) | fio, emmcctl stat |
| 2 | fsync가 장치에 무엇을 요구하고, NAND에 얼마를 더 쓰게 하고, 전원 차단에서 무엇을 지키는가 (강의 25, 27, 35, 52쪽) | logger, tools/blkio.bt, emmc.sh powercut |
| 3 | OTA write를 20 MB/s로 제한했을 때 program suspension이 UI read에 미치는 영향 (강의 44, 45쪽) | cgroup io.max, otawriter, fio, emmcctl trace |

## 프로그램과 도구
```
sudo bash tools/emmc.sh up [key=value ...]   # emmcsim 시작, /dev/nbd0 연결 (conf/tv.conf의 값을 덮어쓴다)
sudo bash tools/emmc.sh mkfs
sudo bash tools/emmc.sh down
sudo bash tools/emmc.sh powercut
sudo bash tools/emmc.sh restore
sudo bash tools/emmc.sh status
./bin/emmcctl stat [raw]
./bin/emmcctl reset
./bin/emmcctl trace [clear]
./bin/emmcctl conf
./bin/emmcctl powercut
./bin/logger    -f FILE [-m line|batch|nosync] [-n lines] [-s line_bytes] [-r lines_per_s] [-b batch_kb] [-t flush_ms] [-y fsync|fdatasync] [-a ackfile] [-w write_progress] [-A]
./bin/logger    -f FILE -P                      # 빈 파일과 부모 디렉터리를 fsync
./bin/logger    -c FILE -a ackfile [-w write_progress]  # 전원 차단 뒤 확인
./bin/otawriter -f FILE -s size_mb [-c chunk_kb] [-r mb_per_s] [-y sync_every_mb] [-d]
sudo bpftrace tools/blkio.bt -c '<command>'
python3 tools/fiosum.py logs/*.json
```

`logger -P`는 전원 차단 전에 빈 파일과 파일 이름을 먼저 durable하게 만든다. `-A`는 기존 파일을 truncate하지 않고 append하며, `-w`는 성공적으로 끝난 마지막 `write_all()` line 번호를 호스트 쪽 sidecar에 기록한다. `-a`는 마지막으로 완료된 fsync의 line 번호를 기록한다. checker는 마지막 write 완료 line, fsync 확인 line, 복구된 연속 line을 출력한다. fsync 확인이 없으면 `NO_FSYNC_ACK`로 표시하며, 이를 `PASS`로 해석하지 않는다.

Lab 3은 erase를 비활성화하고 OTA writer의 `/dev/nbd0` write를 cgroup v2 `io.max`로 최대 20 MB/s 제한한 상태에서 program suspension만 비교한다. 모든 UI latency와 장치 통계는 OTA 시작 직후의 동일한 4초 구간으로 비교하고, 해당 구간의 trace CSV도 저장한다. OTA는 이 구간 뒤에도 완료될 때까지 실행하며, 전체 OTA 완료 시간은 별도로 출력한다. `emmcctl stat`의 die 행은 redo 비율과 누적 redo 시간을 함께 표시한다. 누적 redo 시간은 IPC에서 다시 해야 한 모델 단계 시간의 합이고, OTA 전체 완료 시간과 같을 필요는 없다.

## emmcsim 설정 (conf/tv.conf)
| key | 기본값 | 근거 |
| --- | --- | --- |
| dies, planes, multiplane | 2, 2, 1 | 강의 15, 52, 55쪽. 모델은 multi-plane 동작을 사용 |
| page_kb, pages_per_block | 16, 256 | 강의 15, 16, 55쪽. 256 page는 16쪽의 단순화된 4 MB 블록 예시 |
| t_r_us, t_prog_us, t_bers_us | 120, 1000, 3000 | 강의 16, 41, 54, 55쪽 |
| host_mbps, t_cmd_us | 400, 1 | 강의 39, 41쪽. 명령 1 µs 미만을 모델에서 1 µs로 반올림 |
| nand_mbps | 1600 | 강의 52쪽 (16 KB를 10 µs에 전송) |
| buffer_kb | 16 | 강의 30쪽 (16 KB buffer slot 하나) |
| cache, read_priority | 1, 1 | cache는 강의 30쪽, read-first 정책은 56쪽을 바탕으로 한 시뮬레이터 정책 |
| suspend, suspend_phase_us, suspend_max | off, 40, 0 | suspend 예시는 강의 54쪽. phase 40 µs와 제한 없음은 시뮬레이터 설정값 |
| erase_on_open | 0 | 기본 workload는 새 page에 순차 기록하고 GC를 하지 않으므로 erase 동작을 비활성화 |
| pslc_mb, slc_t_*_us | 0, 25/200/1500 | 강의 14, 31, 42쪽. pSLC 영역은 실험에서 비활성화 |
| read_retry_pct | 0 | 강의 49쪽은 retry 비용을 설명하며, 0은 기준 실험 설정 |
| time_scale | 1 | 모든 장치 시간에 곱하는 배수 |
| size | 1G | RAM 사용량을 줄이기 위한 설정; 강의 TV 예시는 32 GB (55쪽) |

## 주의
- 장치 시간은 모델이지만 fio가 보는 시간에는 nbd 경로와 kernel이 더한 시간이 들어 있다. `emmcctl stat`의 "latency inside the device"가 장치만의 시간이다.
- QD 32에서도 die busy가 낮으면 NBD와 Linux 경로가 병목일 수 있다. `TS=4 bash tools/lab.sh 1c`는 시뮬레이터 내부 동작 시간을 4배로 늘려 장치 병렬성의 영향을 보기 쉽게 하는 선택적 설정이다. 실제 장치가 빨라지는 것은 아니므로, 결과를 비교할 때는 같은 `TS` 값을 사용한다.
- `emmc.sh powercut`은 장치를 바로 없앤다. 쓰던 프로그램은 I/O 오류로 끝나는 것이 정상이다.
- 실습 출력은 logs/에 남는다. 전원 차단 image는 /tmp/emmcsim.img에 남는다.

## 검증 상태
- emmcsim의 timing은 sandbox(x86_64, vCPU 2, kernel 6.18)에서 fio nbd engine으로 강의 예제값과 맞춰 확인했다. 이전 버전의 tools/lab.sh 단계도 QEMU VM(Ubuntu kernel 6.8, kernel nbd, ext4, bpftrace 0.20, cgroup v2)에서 확인했다.
- 사용자가 공유한 WSL 6.6 환경의 3b–3d 실행에서는 OTA 처리량이 모두 약 18.7 MB/s, 전체 시간이 13.70–13.72초였다. UI fio p99는 off/IPS/IPC에서 1,253/461/444 µs, 장치 내부 p99는 1,002/200/180 µs였다. 이는 cgroup v2 io.max=20,000,000 B/s와 고정 4초 UI 측정 구간을 사용한 결과다.
- ZIP의 logs/에 든 수치는 별도 실행에서 수집된 예시라 WSL 결과와 다를 수 있다. 특히 fio latency는 WSL의 NBD·kernel 경로와 실행 환경에 따라 달라지므로, 학생은 각자 실행한 결과를 기록한다.
