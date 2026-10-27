# Assignment 7 materials

Build `vasdump` and complete `libmtrace_lite.so`, then use both to diagnose the
three required cases. The assignment document defines required experiments,
analysis questions, and grading.

## Start here

Requirements: 64-bit Linux/glibc, a C compiler, make, Python 3, binutils,
and strace. Use the same environment for compared runs.

```sh
unzip Assignment7_student_source.zip
cd assignment7
make
make smoke
uname -m
getconf GNU_LIBC_VERSION
```

The case C sources are included in `cases/`. `make` builds all five programs
with your local compiler on ARM64 or x86-64 Linux/glibc. No prebuilt case binaries
are included. ARM64 WSL Ubuntu 26.04 was tested; x86-64 runtime testing has not
been performed. Rebuild with `make clean && make` after switching environments.

`make smoke` checks that the provided infrastructure builds and forwards calls.
It does not certify your statistics or complete the assignment.

## What to edit

| Path | Provided | Your work |
|---|---|---|
| `level1/vasdump.py` | CLI outline and field/category names | Parsing, classification, totals, verbose and diff reports, command lifecycle |
| `level2/support.c` | glibc forwarding, bootstrap, TLS guard, mutex, signal notification | Understand and document it; modify if your design needs it |
| `level2/mtrace_student.c` | Event/report hooks | Counters, pointer ownership, live/peak accounting, histogram, output |
| `workloads/` | ELF, checkpoint and observation examples | Use/adapt for the assignment; still write experiments explicitly described as your own in the specification |
| `tests/` | Four-thread known-count and allocator edge tests | Check semantic behavior and your measured counts |
| `cases/` | Five C sources, common header and standalone Makefile | Build locally; preserve baseline sources and support diagnosis with measurements |
| `tools/sample_status.py` | Timestamped VmSize/VmRSS CSV helper | Add phase interpretation and your own tool evidence |

`vasdump` exits with status 2 until implemented. The tracing skeleton prints
`implementation=student-skeleton complete=no`; this is deliberate. Do not submit
that output as completed statistics. See `level2/README.md` before editing hooks.

## Level 1 workloads

```sh
./observation_workload idle 30
./observation_workload threads
./observation_workload stack 8
./observation_workload stack 64
./observation_workload growth 12
./elf_workload
readelf -lW ./elf_workload
nm -n ./elf_workload
```

ELF, thread and stack modes print a PID/stage and wait for Enter. Inspect from
another terminal before pressing Enter. Growth adds and touches 1 MiB each
second, retains the pointers, and frees them at normal completion. Threads
mode creates four workers that do not explicitly malloc. Recursion touches
4 KiB per frame, with a bounded depth; compile with the supplied flags.

`make elf_workload_nopie` supplies the optional non-PIE comparison.
The ELF workload prints two initialized symbols and both ends of an 8 MiB bss
array, allowing two-symbol load-bias verification and anonymous-tail inspection.

## Checkpoint experiment

Run from this directory; use the relative library path shown below. Dynamic
loader `LD_PRELOAD` lists split on spaces, even when a shell variable is quoted.
Do not put an absolute path containing spaces in `LD_PRELOAD`.

```sh
MTRACE_OUT=connection.trace LD_PRELOAD=./libmtrace_lite.so ./checkpoint_workload
```

From a second terminal at each stage:

```sh
kill -USR1 PID
# Wait for the complete snapshot in connection.trace.
./vasdump PID
```

Press Enter in the first terminal only after both measurements finish.

| Stage | Live workload storage |
|---|---:|
| baseline | 0 bytes |
| A | 1,000 x 1 KiB |
| B | A + 16 MiB |
| C | 1,000 x 1 KiB |
| D | 0 bytes |

All blocks are touched before the stage marker. The program waits at D too;
press Enter once more for normal exit. Runtime allocations can add baseline
bytes: compare stage deltas and explain any additional calls.

## Allocator tests

```sh
LD_PRELOAD=./libmtrace_lite.so ./edge_cases
LD_PRELOAD=./libmtrace_lite.so ./known_count
LD_PRELOAD=./libmtrace_lite.so ./known_count --checkpoints
```

The known-count workload uses four threads with 100,000 malloc/free pairs each.
The optional before/after checkpoints support snapshot deltas. The workload
adds exactly 400,000 malloc calls and 400,000 free calls; thread/runtime cleanup
can add free calls. `EDGE_OK` verifies API behavior, not your statistics.
The Makefile disables builtins so calls are not optimized away.

## Case programs

```sh
./cases/case1 --help
LD_PRELOAD=./libmtrace_lite.so ./cases/case1 > case1.log &
pid=$!
# Wait until case1.log contains READY, then collect evidence.
python3 tools/sample_status.py "$pid" case1.csv --samples 10 --interval 1
wait "$pid"
```

The root launchers `./case1` through `./case5` run the locally compiled programs
and accept the same options, so the assignment document's commands also work.

Record phase markers from the program log alongside CSV. The helper's default
phase label is a reminder, not automatic phase detection. Run a new process
and use new output filenames for each comparison. CSV output refuses overwrite.

| Program | Symptom | Default duration (approximately) | Workload bound |
|---|---|---:|---|
| case1 | Memory grows over time | 13 s | 12 MiB default, 120 MiB maximum |
| case2 | Many objects freed, RSS remains high | 14 s | 32 MiB default, 64 MiB maximum, plus allocator overhead |
| case3 | More threads, large VSZ and small RSS | 37 s | 24 workers default, 64 maximum; virtual stack size follows environment |
| case4 | Repeated large-buffer cycles cost CPU | depends on machine | 32 MiB payload at once by default, 64 MiB maximum |
| case5 | A large reservation barely changes RSS initially | 37 s | 32 MiB default, 64 MiB maximum |

All accept `--ticks 2..120`, `--interval-ms 50..5000`, `--mib 1..64`,
and `--threads 1..64`. Irrelevant options have no effect. Defaults are
ticks=12, interval-ms=1000, mib=32, threads=24. For case3, ticks controls the
steady phase after thread creation; for case4 it controls batch count.

Use defaults for report measurements unless documenting a change. Faster
intervals are useful for debugging, but keep at least 10 samples for trends.
Case2's before_release phase lasts one interval; choose a longer interval if
needed to capture it. Case5 has three observation phases, each lasting ticks
intervals. Case4 has no artificial sampling delay inside its batches.

Programs finish normally after their bounded workload. `kill -TERM PID` stops
early; request a trace snapshot first if you need allocator statistics. Do not
expect an exit summary after default signal termination.

## Evidence

Use `templates/measurements.csv` for the checkpoint table and
`templates/report.md` for a compact report outline. Preserve raw output,
commands, settings, units, and run IDs. These templates contain no measurements
or case answers.
