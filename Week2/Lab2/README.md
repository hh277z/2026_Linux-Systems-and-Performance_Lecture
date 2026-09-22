Lab2/
├── Lab2_instructions.docx  실습 안내와 관측 기록지
├── README.md              파일 설명
├── Makefile               C 프로그램 빌드 규칙
├── check_env.sh           실습 도구 설치와 실행 점검
├── src/                   직접 읽어 볼 C 소스
│   ├── signal_steps.c     Enter로 SIGINT 상태를 단계별 관측
│   └── term_demo.c        SIGTERM의 기본 종료·무시·차단·처리 비교
├── tools/                 관측과 기록을 돕는 도구
│   ├── irq.sh             IRQ 기록 실행과 로그 파일 저장
│   ├── irq_record.sh      kernel의 IRQ 진입·복귀 이벤트 기록
│   ├── irq_summary.py     CPU별 IRQ 이벤트 짝짓기와 요약
│   └── status.sh          지정 PID의 signal 비트 확인
├── samples/               IRQ 관측 실패 시 분석할 실제 예제 로그
├── tests/                 기능 검증 도구
│   └── verify.py          실제 프로세스로 signal 동작 검사
├── bin/                   빌드된 실행 파일
└── logs/                  내 환경에서 수집한 관측 결과