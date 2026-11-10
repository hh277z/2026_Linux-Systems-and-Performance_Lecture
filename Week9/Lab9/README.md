# 9주차 실습: 메모리 압력과 회수

WSL2 Linux에서 진행합니다. 자세한 절차는 `student/Lab9_instructions.docx`와 `student/RUN_GUIDE.md`를 참고하세요.

- `bin/psi_watch`: PSI 원본 읽기와 trigger 관측
- `tools/lab.py normal`, `cpu`, `memory`, `mixed`: 조건별 처리량과 PSI 비교
- `tools/lab.py background`, `global-direct`: background·direct reclaim 관측
- `tools/lab.py file-refault`, `swap-in`: 파일 재접근과 swap-in 비교
- `tools/lab.py high`, `max`: cgroup 메모리 제한과 이벤트 관측
- `Makefile`: `make`로 빌드, `make clean`으로 실행 파일 정리
