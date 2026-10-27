# Assignment 7 case programs

Build on your own **64-bit Linux/glibc** environment (ARM64 or x86-64).
Windows users should run these commands inside WSL. No prebuilt binaries are included.
Requires a C11 compiler, GNU make and pthreads (Ubuntu: `sudo apt install build-essential`).

```sh
make
./case1 --help
./case1
make smoke
```

`case1.c` through `case5.c`, `common.h`, and this Makefile are self-contained.
`make` uses the local compiler without ARM-specific or x86-specific flags.
After changing machines or compiler settings, use `make clean && make`.
You can select a compiler with `make CC=gcc` or `make CC=clang`.

Every program prints `PID=... READY phase=...`. Collect measurements while it runs.
All accept `--ticks 2..120`, `--interval-ms 50..5000`, `--mib 1..64`,
and `--threads 1..64`; defaults are 12, 1000, 32, and 24 respectively.
Options irrelevant to a case have no effect. `make smoke` uses shortened runs;
use defaults for report measurements unless documenting a change.

| Program | Observation | Approximate default duration |
|---|---|---|
| case1 | Memory growth | 13 s |
| case2 | RSS after freeing many objects | 14 s |
| case3 | Thread count, virtual memory and RSS | 37 s |
| case4 | Repeated large-buffer allocation and release | Machine-dependent |
| case5 | Reserved memory and staged page touches | 37 s |

Keep the supplied workload unchanged for baseline measurements. Save any modified
comparison as a separate source file and record its build/run commands.
Support conclusions with measured evidence, even when source inspection suggests a cause.
Page size, glibc version, stack limits and allocator settings can change results;
record `uname -m`, `getconf PAGESIZE`, `getconf GNU_LIBC_VERSION`, and `ulimit -s`.

Build and runtime verification: ARM64 WSL Ubuntu 26.04. x86-64 runtime verification
has not been performed; x86-64 users compile the same sources locally.
