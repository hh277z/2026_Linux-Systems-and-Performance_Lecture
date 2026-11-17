# Assignment 10 실행 안내

## 준비
Linux의 gcc/clang, make, Python 3.10 이상이 필요합니다. WSL 2 또는 지정 VM에서 빌드합니다.
Windows에서 직접 실행하는 exe가 아닙니다. 압축을 푼 뒤 Linux 터미널에서:
```sh
make
python3 student_support/preflight.py > preflight.json
./scenario --help
./scenario M1 --bytes 64KiB --base 64KiB --cycles 4 --period 0.2 --seconds 3
```
마지막 명령은 동작 확인용 작은 부하입니다. 이것만으로 시스템 압박이나 누수의 영향을 입증한 것은 아닙니다.

## 격리 실행
권한 점검 후 root가 cgroup을 준비하고, 실제 부하는 일반 사용자로 실행합니다.
root에서 --uid 0은 거부합니다. 사용자 UID는 `id -u`로 확인하십시오.
```sh
sudo python3 student_support/labrun.py --uid 1000 --out runs/m2-small --max-mib 32 --seconds 10 -- ./scenario M2 --base 8MiB --bytes 40MiB --hold 1 --cycles 2 --period 0.2 --seconds 5
```
이 예는 32MiB 그룹 안에서 의도적인 OOM을 일으킵니다. 정상 완료가 아닌 종료 코드 1이 예상될 수 있습니다.
80MiB로 같은 부하를 반복하면 비교할 수 있습니다. 실험 전에 필요한 여유와 실제 사용량을 확인하십시오.
각 --out은 새 경로여야 합니다. root launcher가 만든 결과를 읽고 필요한 경우 사용자가 소유권을 정리합니다.
부모의 memory controller가 위임되지 않았으면 실행을 중단합니다. 시스템 root controller를 임의로 바꾸지 않습니다.
`observe.py`의 support.csv는 제공용 확인 자료입니다. 학생의 수집기 구현을 대체하지 않습니다.
watchdog는 그룹 안의 부하만 정리하고, 시스템 swap/sysctl/서비스는 건드리지 않습니다.

## 시나리오 인터페이스
M1, M2, M3는 `--bytes --base --cycles --period --hold --seconds --max-bytes --seed`를 받습니다.
크기는 정수 바이트 또는 KiB/MiB/GiB 접미사입니다. 기본 bytes=4MiB, base=8MiB, cycles=12입니다.
기본값은 1분 내 소규모 실행을 위한 시작점입니다. 과제의 5~10분 측정은 cycles, period, seconds와 예산을 함께 조절하십시오.
--seed는 재현 정보로 기록하지만 현재 순차 페이지 접근은 seed로 달라지지 않습니다.
각 프로그램은 실제 페이지를 쓰며, 시작/주기/종료를 공통 monotonic 시각으로 출력합니다.
--max-bytes는 누적 보유를 포함한 상한이며 최대 1GiB입니다. M1에서 cycles x bytes + base가 이 상한을 넘으면 거부합니다.
소스 분석은 허용됩니다.
M2/M3는 cgroup 설정과 함께 해석해야 합니다. M3 단독 실행이나 작은 기본값이 system PSI 증가를 보장하지 않습니다.

## LD_PRELOAD
학생이 만든 라이브러리는 다음과 같이 상대 경로로 지정합니다.
```sh
LD_PRELOAD=./alloctrace.so ./scenario M1
```
LD_PRELOAD 값은 동적 로더가 공백으로 분리합니다. 공백이 포함된 절대 경로는 쉘에서 따옴표로 감싸도 실패할 수 있습니다.
라이브러리가 있는 폴더에서 공백 없는 상대 경로를 쓰거나 Linux 홈 아래 공백 없는 경로를 사용하십시오.
SIGUSR1은 덤프 요청이며, 안전한 처리 시점에 출력합니다. SIGKILL 후 종료 덤프를 기대하지 마십시오.

## earlyoom
vendor/earlyoom은 공식 v1.9.0 원본이며 라이선스와 출처/해시를 포함합니다.
```sh
python3 student_support/build_earlyoom.py --policy starter/lmk_policy.c --output build/earlyoom
./build/earlyoom/earlyoom -v
```
기본 빌드는 원본 코드 경로와 별도의 --lab 경로를 만듭니다. 자동 설치/서비스 실행은 하지 않습니다.
--lab은 등록부 안의 lab10-* 그룹 프로세스만 후보로 삼고, pidfd를 지원하지 않으면 일반 PID kill로 후퇴하지 않습니다.
등록부는 Linux /tmp의 보호된 파일로 생성합니다. Windows 마운트의 chmod 표시만으로 보호를 가정하지 않습니다.
labrun.py의 registry-path.txt는 실행 중인 등록부 위치를 가리킵니다. registry.tsv는 제출용 복사본입니다.
--lab 옵션은 도움말 오류에 나오는 제한된 옵션 집합입니다. 원본 CLI와 구분하십시오.
정책 파일의 PSI/중요도 선택 부분은 학생 구현 과제이며 미완성 상태에서 명시적으로 실패합니다.

## 보드 실험
관리자가 지정한 부모 cgroup, 보드용으로 빌드한 memhog/foreground/earlyoom 경로를 사용합니다.
board_run.py --help에서 실제 인자를 확인하십시오. --config 1~4, --some-us, --full-us, --grow-rate, --burst를 지원합니다.
주 비교는 some/full trigger를 함께 등록하고, some 임계값 screening에서는 --full-us 0으로 full을 비활성화합니다.
기본 프로필은 검증할 시작점입니다. --burst가 붙었다는 사실만으로 benign 검증이 완료된 것은 아닙니다.
보드 swap이나 기존 memory manager를 자동으로 바꾸지 않습니다. safety cgroup이 먼저 OOM을 일으킬 수 있으며, system PSI나 earlyoom kill이 반드시 발생하는 것은 아닙니다.
실제 보드에서 크기/증가율/시간/관리자 공존을 검증한 뒤 비교 조건을 고정하십시오.

