# Assignment 6 skeleton

Read this together with the assignment document distributed alongside this archive. Everything builds and runs as it is. The Level 1 and 2 TODO functions are stubs, so their tests fail until you implement them. The Level 3 case files are working code that can deadlock: that is the starting point.

```
make                                  # bin/bqtest, bin/bqtest_tsan, bin/albumsvc,
                                      # bin/devsvc, bin/devsvc_tsan, bin/devsvc_ref, bin/lockcheck.so
bash tools/l1_check.sh                # Level 1: close, timeout and stress tests, normal and TSan builds
bash tools/l2_run.sh lock             # Level 2: albumsvc + lockwait.bt + perf sched (asks for sudo once)
bash tools/l3_run.sh 1                # Level 3: case 1 with lockcheck.so, then the TSan build (diagnosis)
bash tools/l3_check.sh 1              # Level 3: the five pass criteria of case 1
```

| File | What you do |
| --- | --- |
| src/bq.c | TODO(L1-1 to 4) bq_put, bq_get, bq_timed_get, bq_close (contract in src/bq.h) |
| src/album.c | TODO(L2-1) album_snapshot, album_snapshot_free |
| src/albumsvc.c | TODO(L2-2 to 5) snapshot saver, handoff send, apply and save |
| src/sink.c | TODO(L3-1) case 1: the order of the two sink locks |
| src/bus.c | TODO(L3-2) case 2: callbacks that call back into the bus |
| src/catalog.c | TODO(L3-3) case 3: rwlocks and their modes |
| src/config.c | TODO(L3-4) case 4: the config read with RCU, freed after a grace period |
| src/bqtest.c, src/common.h, src/bq.h, src/album.h | Provided. Do not modify |
| src/devsvc.c, src/devsvc.h, src/rcu.c, src/rcu.h | Provided Level 3 driver and RCU. Do not modify |
| tools/ (including tools/lockcheck.c) | Provided. Do not modify |
| lib/libbq_ref.a | A working bq for Level 2 if Level 1 is not finished (see below) |
| lib/libdevsvc_ref.a | The reference fixes of the four cases, linked into bin/devsvc_ref for the speed criterion |

## Notes

- Build with the Makefile. It exports function and global variable names (`-rdynamic`), so lockcheck can print where a lock was taken and the name of a global lock. Static names are not exported: lockcheck then prints `prog+0xOFFSET` for a code address (`addr2line -f -e bin/prog 0xOFFSET` gives the function and line) and `mutex@0x...` or `rwlock@0x...` for a lock.
- Measure time only with the normal build. TSan builds are for finding races and lock misuse.
- lockcheck and TSan are separate runs. Do not load lockcheck.so into a TSan binary.
- If a TSan binary stops with `unexpected memory mapping`, run `sudo sysctl vm.mmap_rnd_bits=28`.
- bin/devsvc stops a run that hangs: when a worker finishes no request for 3 s, it prints what every worker was doing and `result: HANG`.
- The close and producer-wakeup tests wait for the queue wait counters before triggering close or dequeue; they do not rely on a fixed startup sleep.
- l3_run.sh runs TSan with its own lock order check. l3_check.sh turns that check off (`detect_deadlocks=0`) and leaves lock order to lockcheck.
- `TAG=name` changes the log names of l2_run.sh, l3_run.sh and l3_check.sh, so a second run does not overwrite the first.
- Level 1 not finished: `make albumsvc_ref`, then `ALBUMSVC=./bin/albumsvc_ref bash tools/l2_run.sh ...`. Say so in your report.
- l2_run.sh asks for the sudo password once and then runs as root. Type one command at a time.
- futex.bt and lockwait.bt put uprobes on libc functions, which slows the traced program down. Keep `-n` small (20000) for futex.bt runs.
- perf data goes to /tmp.
