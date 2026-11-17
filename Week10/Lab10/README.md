# 10주차 실습: 메모리 문제의 원인 찾기

WSL2 Ubuntu에서 진행합니다. 자세한 절차는 `instructions.docx`를 참고하세요.

- `lab10_a`: cache entry 해제 누락으로 발생하는 memory leak과 수정 버전 비교
- `lab10_b`: 재생 중 메모리 사용량 증가와 cgroup OOM 관측
- `lab10_c`: 새 서비스 추가에 따른 memory pressure와 `memory.high` 조정 효과 관측
- `lab10_view`: 실제 메모리 사용량과 PSI를 실시간으로 표시
- `lab_env.sh`: 실습용 cgroup 준비와 정리
- `Makefile`: `make`로 네 실행 파일 빌드

## 빌드

gcc, make, procps, util-linux, heaptrack이 필요합니다. 압축을 푼 실습 폴더에서 실행하세요.

```bash
make
```

## 실습 준비와 실행

실습은 A → B → C 순서로 진행합니다. 각 실습이 끝나면 아래의 정리 명령을 실행한 뒤 다음 실습을 준비하세요.

**실습 A**

```bash
sudo bash lab_env.sh setup A
source state/session.env
sudo ./lab10_a buggy "$CG/media"
```

**실습 B** — 첫 실행은 설정된 한도로 인해 OOM으로 종료됩니다.

```bash
sudo bash lab_env.sh setup B
source state/session.env
echo 96M | sudo tee "$CG/media/memory.max"
sudo ./lab10_b "$CG/media"
```

**실습 C** — 기존 서비스를 먼저 실행하고, 약 5초 뒤 새 서비스를 추가합니다.

```bash
sudo bash lab_env.sh setup C
source state/session.env
sudo ./lab10_c existing "$CG/existing" > existing.log &
sleep 5
sudo ./lab10_c new "$CG/new" > new.log &
```

A와 B는 각 STAGE에서 Enter를 기다립니다. 명령 끝에 `--demo`를 붙이면 중간 대기 없이 실행됩니다. A의 수정 버전은 `buggy`를 `fixed`로 바꿔 실행합니다. 설정 변경과 원본 관측 명령은 안내서를 따라 진행하세요.

실시간 화면은 준비가 끝난 뒤 같은 실습 폴더의 다른 터미널에서 실행합니다. C에서 작업 시간도 보려면 마지막 명령 끝에 `existing.log new.log`를 추가하세요.

```bash
source state/session.env
sudo ./lab10_view "$CG"
```

## 실습 정리

viewer는 Ctrl+C로 종료합니다. 다음 명령은 실행 중인 실습 프로그램과 cgroup을 정리합니다.

```bash
sudo bash lab_env.sh cleanup
```
