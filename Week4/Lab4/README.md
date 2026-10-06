# Week 4: CPU 구조와 cache (cache 계층, false sharing, jank case study: perf sched + nice + cgroup)

| 경로 | 설명 |
| --- | --- |
| Lab4_instructions.docx | 실습 안내와 관측 기록지 |
| Makefile | ws_demo, fs_demo, mtwork 빌드 (-g, frame pointer 유지) |
| check_env.sh | perf sched, cgroup v2 cpu·cpuset, lscpu cache 크기 점검 |
| src/ws_demo.c | working set sweep: random pointer chase, 크기별 ns/access |
| src/fs_demo.c | false sharing: 인접 counter vs cache line padding, CPU 지정 |
| src/mtwork.c | pthread worker (-n 이름, busy/periodic/sysio, cpu=, sched=, -c cgroup). renderer/indexer 역할, Week 6에서도 사용 |
| tools/perf_jank.sh | perf sched record -a → latency(victim) → timehist(culprit) 자동 추출 |
| tools/fixcg.sh | lab/fg, lab/bg cgroup: cpu.weight, cpu.max, cpuset.cpus 로 간섭 task 제어 |
| tools/perf_sched.sh | perf sched record / latency / timehist (명령 단위) |
| tools/perf_profile.sh | cpu-clock sampling, thread별 report, on-CPU flame graph |
| tools/flamegraph.pl, stackcollapse-perf.pl | FlameGraph (CDDL) |
