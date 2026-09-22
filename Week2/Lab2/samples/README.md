# 실제 기록된 IRQ 예제

이 폴더는 수강생이 직접 측정한 결과가 아니라 제작 환경에서 얻은 실제 로그다.
합성·가상 숫자가 아니다. 직접 관측이 어려울 때 **제공 예제 분석**으로 표시하고 사용한다.

- 캡처 일시(UTC): `irq-trace.txt` 첫 줄에 기록.
- 환경: WSL 2, Ubuntu 26.04, aarch64, Linux 6.18.33.2-microsoft-standard-WSL2.
- 도구: 이 배포의 `tools/irq_record.sh`, 별도 tracefs instance, 전 CPU 3초.
- 이벤트: irq:irq_handler_entry / irq:irq_handler_exit.
- 기록된 entry 1167개, 대응 pair 1167개, 미대응 0개.
- per-CPU overrun, commit overrun, dropped events는 모두 0.
- 첫 pair: CPU 000, IRQ 13, Hyper-V VMbus, 2478.712843 → 2478.712873초, 30μs.

```bash
python3 tools/irq_summary.py samples/irq-trace.txt
```

`recorder.txt`에는 유실 통계, `interrupts-before.txt`와 `interrupts-after.txt`에는 누적 카운터가 있다.
두 카운터의 측정 구간은 sudo 준비와 파일 저장 시간도 포함하므로 trace의 3초 구간과 정확히 같지 않다.
tracepoint는 handler action 단위이며 /proc/interrupts의 집계 범위와 항상 일치하지 않는다.
이 예제의 IRQ 종류·번호·횟수를 모든 수강생 환경의 정답으로 사용하지 않는다.
