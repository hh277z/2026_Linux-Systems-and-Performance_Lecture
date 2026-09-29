실습 자료 디렉터리
week3_lab/
├── Lab3_instructions.docx   안내서와 관측 기록지
├── Makefile / check_env.sh  빌드와 환경 확인
├── src/
│   ├── wait_modes.c         실습 1-1 완성 코드
│   ├── fd_scale.c           실습 1-2 학생 코드
│   ├── trigger_steps.c      LT와 ET 단계별 비교
│   ├── ipc_server.c         스레드 하나로 동작하는 UDS 서버
│   ├── log_stats.c/.h       두 로그 집계 알고리즘
│   └── lab.h               공통 보조 함수
├── src_answer/             강사용 실습 1-2 정답
├── service_log_dataset/    합성 로그·정답 통계 (200만 행)
├── tools/                  실습 명령을 묶어 둔 보조 도구
└── logs/                   실행·관측 결과