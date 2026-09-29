# 합성 서비스 요청 로그 데이터셋 (Linux C 프로그래밍 수업용)

단일 스레드 이벤트 루프 서버가 "로그 통계 집계" 요청을 처리하는 동안
다른 클라이언트의 PING 응답이 지연되는 상황을 관측하고, `perf`로 CPU
병목 함수를 찾는 실습을 위한 합성 데이터와 생성/검증 스크립트입니다.

- 기본 구현: 서비스 하나의 통계를 낼 때마다 전체 로그를 다시 순회
  → O(서비스 수 × 로그 행 수)
- 개선 구현: 전체 로그를 한 번만 순회하며 서비스 ID별 배열에 누적
  → O(로그 행 수 + 서비스 수)

`duration_us`는 과거 요청의 **기록값**일 뿐입니다. 생성 스크립트도,
실습 서버도 이 값만큼 실제로 sleep 하지 않습니다. 실습에서 체감되는
지연은 이 로그를 검색·집계하는 연산 자체에서 나와야 합니다.

## 1. 파일 목록

| 파일 | 설명 |
|---|---|
| `requests.csv` | 기본 실습 데이터 (2,000,000행, 서비스 1,024개, seed=42) |
| `expected_stats.csv` | `requests.csv`에 대한 정답 통계 (서비스별) |
| `generate_logs.py` | 재현 가능한 합성 로그 생성기 (표준 라이브러리만 사용) |
| `verify_logs.py` | `requests.csv`를 다시 읽어 형식/통계를 독립적으로 검증 |
| `SHA256SUMS.txt` | 아래 4개 파일의 SHA-256 체크섬 |

## 2. CSV 형식

파일명: `requests.csv`

```
timestamp_ms,service_id,status,duration_us
1767225600002,782,200,7193
1767225600002,352,200,1150
1767225600003,844,200,680
```

- `timestamp_ms`: Unix epoch ms, 양의 정수, 파일 전체에서 비감소.
- `service_id`: 0 ~ (services-1) 정수. 기본 데이터는 0~1023, 모든 서비스가 최소 1회 등장.
- `status`: `200,201,204,400,404,429,500,503` 중 하나 (대부분 성공, 소수 오류).
- `duration_us`: 1 ~ 10,000,000 범위의 정수 마이크로초.

형식 제약: UTF-8, BOM 없음, LF 개행, 쉼표 구분, 모든 데이터 필드는 정수,
따옴표/천단위 구분자/소수점/빈 필드/주석/빈 줄 없음, 헤더는 1회만,
`service_id`로 정렬하지 않음(여러 서비스가 시간순으로 섞여 있음).

## 3. 데이터 분포 특징

- 서비스별 인기도는 로그정규분포 가중치로 결정 → 일부 서비스에 요청 집중, 그러나 모든 서비스에 충분한 요청 존재.
- 서비스별로 처리시간 분포(평균/분산)가 서로 다름 → 로그정규분포 기반, 대부분 짧고 일부 긴 처리시간이 섞임.
- `429/503/500` 오류는 처리시간이 추가로 길어지도록 반영.
- 전체 파일에 사인파 형태의 시간대별 요청량(속도) 변화 적용.
- 무작위로 선택된 짧은 구간·서비스 부분집합에 대해 오류율이 급증하는 "오류 집중 구간(burst)"을 여러 개 삽입.
- 실제 사람/회사 로그가 아닌 100% 합성 데이터.

## 4. 생성 명령

표준 라이브러리만 사용하며, 행을 순차적으로 스트리밍 기록하므로 전체를
메모리에 쌓지 않습니다. 동일한 인자+seed는 항상 동일한 CSV를 만듭니다.

```bash
# 빠른 기능 확인 (약 1초)
python3 generate_logs.py --rows 100000 --services 1024 --seed 42 --output requests_small.csv

# 기본 실습 데이터 (이 저장소에 포함된 것과 동일, 약 8초)
python3 generate_logs.py --rows 2000000 --services 1024 --seed 42 --output requests.csv --overwrite

# 지연이 너무 짧게 느껴지는 빠른 PC용 (더 큰 데이터)
python3 generate_logs.py --rows 8000000 --services 1024 --seed 42 --output requests_large.csv
```

옵션 설명:
- `--rows`: 데이터 행 수 (헤더 제외). `--services` 이상이어야 함.
- `--services`: 서비스 ID 개수 (0 ~ services-1). 변경 가능.
- `--seed`: 재현성 시드.
- `--output`: 출력 CSV 경로.
- `--overwrite`: 기존 출력 파일을 덮어쓰려면 명시적으로 지정해야 함 (안전장치).

실행 시간은 CPU 성능에 따라 달라지므로 "몇 초 걸린다"는 보장은 하지
않으며, `--rows` 값으로 원하는 지연 크기를 조절하면 됩니다.

## 5. 검증 명령

`verify_logs.py`는 생성기가 계산한 값을 신뢰하지 않고, 저장된 CSV를
처음부터 다시 읽어 다음을 확인합니다:

1. 헤더/필드 개수/정수 형식/값 범위(status, service_id, duration_us) 검사
2. `timestamp_ms` 비감소 여부 검사
3. 행 수가 `--expected-rows`와 일치하는지 확인 (옵션)
4. 서비스별 통계를 처음부터 재계산해 `expected_stats.csv`와 완전히 일치하는지 비교
5. 서비스별 `request_count` 합계가 전체 행 수와 같은지 확인

```bash
python3 verify_logs.py \
  --csv requests.csv \
  --stats expected_stats.csv \
  --services 1024 \
  --expected-rows 2000000
```

성공 시 `OK: all checks passed`와 종료 코드 0, 실패 시 `FAIL: ...` 메시지와
0이 아닌 종료 코드를 반환합니다. 이 저장소에 포함된 `requests.csv` /
`expected_stats.csv` 조합은 이미 위 명령으로 실행 및 검증되었습니다
(아래 실행 로그 참고).

## 6. expected_stats.csv 형식

```
service_id,request_count,error_count,total_duration_us,min_duration_us,max_duration_us
```

- `service_id` 오름차순, 0~1023 모두 한 줄씩 등장.
- `error_count`: `status >= 400`인 행 수.
- 합계는 파이썬 임의 정밀도 정수로 계산되어 오버플로 없음.
- 평균 처리시간이 필요하면 `total_duration_us / request_count`로 계산 (별도 실수 필드 없음).

## 7. 실제 실행 결과 (이 환경에서 수행)

```
$ python3 generate_logs.py --rows 2000000 --services 1024 --seed 42 --output requests.csv --overwrite
Generating 2000000 rows across 1024 services (seed=42) -> requests.csv
Done. Last timestamp_ms = 1767228196803
(real 0m8.3s)

$ python3 verify_logs.py --csv requests.csv --stats expected_stats.csv --services 1024 --expected-rows 2000000
OK: all checks passed
  rows checked        : 2000000
  services checked    : 1024
  total_duration sum  : 7248802900
(EXIT=0, real 0m4.0s)
```

## 8. 파일 크기 및 SHA-256

| 파일 | 크기 |
|---|---|
| `requests.csv` | 약 53.1 MB (2,000,000행) |
| `expected_stats.csv` | 약 28.4 KB (1,024행) |
| `generate_logs.py` | 약 10 KB |
| `verify_logs.py` | 약 6.2 KB |

SHA-256 체크섬은 `SHA256SUMS.txt`에 있으며, 확인 방법:

```bash
sha256sum -c SHA256SUMS.txt
```
