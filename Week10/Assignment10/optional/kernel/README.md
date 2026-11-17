# M4 M5 커널 메모리 진단

제공된 `lab10mod.c`를 지정된 실습 VM의 커널에 맞춰 빌드하고 관측합니다.
새 커널 모듈을 처음부터 구현하는 과제가 아닙니다. M1~M3를 마친 뒤 수행하는 선택 실험입니다.
두 시나리오는 하나의 모듈에서 `scenario=4` 또는 `scenario=5`로 선택합니다.

## 실행 환경

이 실험은 강의에서 지정한 폐기 가능한 Linux VM에서만 수행합니다.
해당 커널의 빌드 헤더, 모듈 로딩 권한과 debugfs가 필요합니다.
WSL이나 개발 보드에서 임의로 모듈을 로드하지 마십시오.
지정 VM 또는 필요한 커널 기능을 제공받지 못했다면 실행하지 않아도 됩니다.

```sh
make KDIR=/lib/modules/$(uname -r)/build
```

Linux 홈 아래 공백이 없는 작업 경로를 사용하십시오.

## M4 연속 페이지 할당

```sh
sudo insmod ./lab10mod.ko scenario=4 limit_mib=8 interval_ms=1000
sudo cat /sys/kernel/debug/lab10/status
cat /proc/buddyinfo
sudo rmmod lab10mod
```

모듈은 order-5 연속 블록을 확보하고 개별 페이지로 나눈 뒤 짝수 위치의 페이지를 반환합니다.
각 블록 내부의 물리적 인접 관계는 유지되지만, 시스템의 다른 가용 영역까지 통제하지는 않습니다.
이후 order-4 할당을 `probe_limit`회 시도합니다. 기본값은 60회이며 완료 후 `done=1`이 됩니다.
`probe_attempts`, `probe_successes`, `probe_failures`와 zone별 buddyinfo를 함께 기록하십시오.
`layout_blocks=0`이면 배치를 준비하지 못한 것이므로 단편화 배치가 만들어졌다고 해석하면 안 됩니다.
`layout_failures`는 배치 준비 실패, `probe_failures`는 실제 order-4 요청 실패를 구분합니다.
`page_slots`는 초기 확보 페이지 수, `live_pages`는 현재 보유 페이지 수입니다.
`released_pages`와 합치면 `page_slots`가 됩니다. `live_payload_bytes`는 현재 페이지 보유량입니다.
이 부하가 단편화나 할당 실패를 반드시 재현하는 것은 아닙니다. 할당 성공도 실제 결과로 기록합니다.
실패가 관측되면 사용 가능한 zone, 높은 order의 free block과 compaction 전후를 비교하십시오.
수동 compaction은 지정 VM에서 해당 기능을 지원하는 경우에만 수행합니다.

## M5 커널 객체 보유

```sh
sudo insmod ./lab10mod.ko scenario=5 limit_mib=8 interval_ms=1000 retain=1
sudo cat /sys/kernel/debug/lab10/status
grep -E 'MemAvailable|Slab|SReclaimable|SUnreclaim' /proc/meminfo
sudo rmmod lab10mod
```

두 retain 조건은 모두 `limit_mib * 1024 * 1024 / 256`회의 할당 시도를 수행합니다.
실패도 한 번의 시도로 집계하며, 기본값 8MiB에서는 두 조건 모두 32,768회입니다.
매 주기에 최대 1,024회를 수행한 뒤 공통 시도 예산에 도달하면 `done=1`이 되고 작업을 멈춥니다.
`allocation_attempts/successes/failures`, `free_count`, `live_objects`를 기록하십시오.
작업 중 `allocation_successes = free_count + live_objects` 관계를 확인할 수 있습니다.
객체 보유량은 `limit_mib`에서 제한되며, 모듈 제거 시 보유한 객체를 모두 반환합니다.
`live_payload_bytes`에는 포인터 테이블과 slab 관리·디버그 오버헤드가 포함되지 않습니다.
프로세스 PSS와 Slab/SUnreclaim 추세를 비교하고, 지원되는 SLUB 추적으로 할당 지점을 확인하십시오.
추적에는 지정 VM의 CONFIG_SLUB_DEBUG, cache tracking 설정과 debugfs가 필요할 수 있습니다.
LD_PRELOAD는 커널 할당을 추적하지 않습니다.

`retain=0`은 배치마다 객체를 해제하는 비교 조건입니다. 모듈을 제거한 뒤 다른 조건으로 다시 로드하고,
같은 기간·할당량에서 객체 수와 메모리 지표를 비교하십시오.
두 조건 모두 `done=1`과 같은 `allocation_attempts`를 확인하고 실패 횟수도 함께 비교하십시오.
작업 완료 후 보유 객체는 관측을 위해 남아 있으며, 이 구간의 평탄한 그래프만으로 해제 여부를 판단하지 마십시오.
환경에 따라 kmalloc cache 이름이나 변화량이 다를 수 있습니다.

## 제출

실행 환경, 실제 명령, 관측 시간과 원본 로그, 원인 가설, 추가 확인 및 비교 결과를 제출합니다.
M4의 성공·실패 결과를 예측값으로 대체하지 마십시오. 매 실험 종료 후 모듈 제거를 확인하십시오.
