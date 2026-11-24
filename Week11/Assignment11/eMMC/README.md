# Assignment 11 스켈레톤: eMMC 안에서 정해지는 I/O 시간

과제 명세서는 배포 파일 루트의 `LG_Assignment11_Final_Storage.docx`에 있다. 장치 모델 emmcsim과 측정 도구는 최신 Lab 11과 같다.

## 준비
```
bash check_env.sh                 # Lab 11과 같은 점검
make                              # bin/emmcsim.so, emmcctl, logger, otawriter, uiburst
source tools/params.sh                 # 공통 실험 설정
```

## 구현할 파일
| 파일 | 구현할 부분 |
| --- | --- |
| tools/l1_sweep.sh | 새로 작성. tools/params.sh를 불러오고 과제 5.1의 설정을 tools/l1_point.sh로 잰다 |
| src/apps/logpolicy.c | TODO(L2-1~4). 명세는 src/apps/logpolicy.h |
| src/apps/otapolicy.c | TODO(L3-1~3). 명세는 src/apps/otapolicy.h |

제공된 skeleton code는 그대로 빌드되고 실행된다. logpolicy.c의 stub은 line마다 write와 sync를 하고, otapolicy.c의 stub은 기다리지 않는다. 그래서 구현하기 전까지 l2_check와 l3_check는 실패한다.

## 실행과 검사
```
sudo bash tools/l1_point.sh <name> [key=value ...]     # 장치 설정 하나를 재고 logs/l1/results.csv에 한 줄
sudo bash tools/l1_check.sh
sudo bash tools/l2_run.sh <name> [key=value ...] -- <logger args>
sudo bash tools/l2_check.sh
sudo bash tools/l3_run.sh <name> <burst reads> <period ms> [key=value ...] -- <otawriter args>
sudo bash tools/l3_check.sh <policy args>    # nbd 경로가 느리면 sudo TS=4 bash ...
```
run과 check script는 실행할 때마다 emmcsim을 새로 띄운다. 결과는 logs/l1, logs/l2, logs/l3에 남는다.

## 고치지 않는 파일
src/emmcsim/, conf/, src/apps/의 logger.c, otawriter.c, uiburst.c, logpolicy.h, otapolicy.h, 그리고 tools/의 제공 script.
