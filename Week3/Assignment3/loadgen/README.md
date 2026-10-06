# Assignment 3: Load Generator

A C tool for testing your event-driven server on Linux/WSL. No runtime dependencies beyond standard Linux libraries.

## Build and run

In an Ubuntu/WSL terminal, open this folder and run:

```bash
sudo apt update
sudo apt install build-essential
make
```

Start your server in another terminal. Use `127.0.0.1` when both programs run in the same WSL distribution.

```bash
./loadgen <server-ip> <port> <clients> <requests-per-client> <workload> [options]
./loadgen 127.0.0.1 9090 32 1000 echo
./loadgen --help
```

## Workloads

| Workload | Purpose |
| --- | --- |
| `functional` | 11 checks: split/combined requests, client buffer isolation, size limits, quit, and disconnects |
| `echo` | Baseline throughput and latency; verify each response |
| `time` | Test lightweight time requests |
| `compute` | Measure worker scaling and overload |
| `mixed` | Mix compute with echo/time; default compute share: 20% |
| `responsive` | Send one compute request while other clients send echo/time |

`functional` ignores requests-per-client and uses at least 3 connections. Invalid-input checks verify that the server continues serving new clients.
The isolation check keeps one request incomplete while another client sends a different request.
`responsive` requires at least 2 clients; client 0 sends compute once.

## Experiments

```bash
mkdir -p results

# Level 1: functionality and baseline
./loadgen 127.0.0.1 9090 8 1 functional
./loadgen 127.0.0.1 9090 32 1000 echo --json results/baseline.json

# Level 2: run on both blocking and offload versions
./loadgen 127.0.0.1 9090 2 300 responsive \
  --work 100000000 --timeout 30 --interval-ms 5 \
  --label blocking --json results/blocking.json

# Level 3: repeat with different server worker counts and queue capacities
./loadgen 127.0.0.1 9090 64 200 compute \
  --work 1000000 --timeout 30 --max-seconds 180 \
  --label 'workers=4,queue=16' --json results/w4-q16.json
```

For Level 2, choose a `--work` value that takes about 0.3-1 second, then keep it fixed for both versions. Change the label and output filename for each run.
For Level 3, change workers or queue capacity **on your server**, keeping the other settings and load fixed.

Repeat each configuration at least 3 times and report medians. The optional summary tool requires Python 3:

```bash
for run in 1 2 3; do
  ./loadgen 127.0.0.1 9090 32 1000 echo \
    --label baseline --json "results/baseline-${run}.json" || break
done
python3 tools/summarize.py results/baseline-*.json > results/summary.csv
```

The summary tool requires at least 3 successful runs per configuration. Use distinct labels for different server settings.

## Server protocol

These are the response formats checked by this tool; the assignment PDF gives examples.
Each request and response ends with `\n`. Responses must be printable ASCII, at most 256 bytes including the newline; CRLF is also accepted.

| Request | Expected response |
| --- | --- |
| `echo <text>` | `OK <same text>` |
| `time` | `TIME YYYY-MM-DD HH:MM:SS` with a valid date and time |
| `compute <work>` | `RESULT <nonempty output>` or `BUSY` |
| `quit` | `BYE`, then close the connection |

`work` is a positive integer, not a duration. Use the same compute implementation and value throughout comparisons.

## Reading results

- **`success_rps`**: successful requests per second; excludes BUSY.
- **`busy_percent_of_responses`**: BUSY / (success + BUSY). Errors are reported separately.
- **`latency_ms`**: mean, p50, p95, p99, and maximum; success and BUSY are separate.
- **`not_sent` / `errors`**: unfinished requests and failed attempts; both should be zero for a complete run.
- **`responsive.light_overlap_success_latency_ms`**: light-request latency for requests started before the compute response arrived. Compare this between blocking and offload.

Latency includes sending, server processing, and receiving the full response. Connection setup is excluded from throughput timing.
Each client waits for a response before sending its next request: this is **closed-loop load**, not a fixed requests-per-second input.

Exit codes: `0` valid run (BUSY allowed), `1` failure or inconclusive experiment, `2` usage/setup error, `130` interrupted. An all-BUSY run does not demonstrate successful compute processing.

Use `--timeout` for slow requests and `--max-seconds` for the overall run limit. See `--help` for all options.
JSON files are overwritten. Record server settings and build options alongside results.

Server CPU/perf observations and checks of epoll, timerfd, eventfd, synchronization, and compute correctness still require server-side evidence.
