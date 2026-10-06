#!/usr/bin/env bash
# jank case study (slide 52):
# 1) record system-wide scheduling events
# 2) find renderer's worst scheduling delay in timehist
# 3) match it to perf sched latency's Max delay
# 4) show tasks that ran on the same CPU immediately before it
#
# usage:
#   bash tools/perf_jank.sh <tag> [seconds] [victim_comm]
#
# default:
#   seconds = 8
#   victim_comm = renderer

set -eu

tag=${1:?usage: perf_jank.sh <tag> [seconds] [victim_comm]}
secs=${2:-8}
victim=${3:-renderer}

mkdir -p logs

data="/tmp/perf-jank-$tag.data"
latency="logs/perf-jank-$tag-latency.txt"
timehist="logs/perf-jank-$tag-timehist.txt"


# ------------------------------------------------------------
# Record scheduling events
# ------------------------------------------------------------
sudo perf sched record -a -o "$data" -- sleep "$secs"

sudo perf sched latency -i "$data" --sort max > "$latency"
sudo perf sched timehist -i "$data" > "$timehist"

sudo chown "$(id -u):$(id -g)" "$latency" "$timehist"


# ------------------------------------------------------------
# Formatting helpers
# ------------------------------------------------------------
print_timehist_header()
{
    printf "%-14s %-7s %-32s %10s %14s %10s\n" \
        "time" "cpu" "task" "wait(ms)" "sch_delay(ms)" "run(ms)"

    printf "%-14s %-7s %-32s %10s %14s %10s\n" \
        "--------------" "-------" "--------------------------------" \
        "----------" "--------------" "----------"
}

format_timehist()
{
    awk '{
        printf "%-14s %-7s %-32s %10.3f %14.3f %10.3f\n", \
            $1, $2, $3, $4, $5, $6
    }'
}


# ------------------------------------------------------------
# Find victim's worst scheduling delay from timehist
#
# timehist columns:
# time  cpu  task  wait_time  sch_delay  run_time
# ------------------------------------------------------------
worst=$(awk -v v="$victim" '
$3 ~ v && $5+0 > max {
    max = $5 + 0
    line = $0
}
END {
    print line
}
' "$timehist")

if [ -z "$worst" ]; then
    echo "ERROR: victim '$victim' not found in timehist"
    sudo rm -f "$data"
    exit 1
fi

worst_time=$(echo "$worst" | awk '{print $1}')
worst_cpu=$(echo "$worst" | awk '{print $2}')
worst_delay=$(echo "$worst" | awk '{print $5}')


# ------------------------------------------------------------
# 1. Match timehist sch delay to perf sched latency Max delay
#
# WSL may print broken/empty task names in perf sched latency.
# Therefore, identify the victim row using the Max delay value.
# ------------------------------------------------------------
echo "== 1. perf sched latency: $victim Max delay =="

victim_lat=$(awk -v d="$worst_delay" '
{
    for (i = 1; i <= NF; i++) {
        if ($i == "max:") {
            val = $(i+1) + 0

            # Allow small rounding difference.
            diff = val - d
            if (diff < 0)
                diff = -diff

            if (diff < 0.001) {
                print
                exit
            }
        }
    }
}
' "$latency")

if [ -n "$victim_lat" ]; then
    echo "$victim_lat"
else
    echo "(matching latency row not found)"
fi


# ------------------------------------------------------------
# 2. Show victim's worst scheduling-delay event
# ------------------------------------------------------------
echo
echo "== 2. worst sch delay of $victim =="

print_timehist_header
echo "$worst" | format_timehist


# ------------------------------------------------------------
# 3. Show recent events on the same CPU
#
# Count only events from the target CPU so the ring buffer
# preserves the correct same-CPU order.
# ------------------------------------------------------------
echo
echo "== 3. tasks running on the same CPU before $worst_time =="

print_timehist_header

awk -v t="$worst_time" -v cpu="$worst_cpu" '
$2 == cpu {
    n++
    history[n % 5] = $0

    if ($1 == t) {
        for (i = n - 4; i <= n; i++) {
            if (i > 0)
                print history[i % 5]
        }
        exit
    }
}
' "$timehist" | format_timehist


echo
echo "Saved:"
echo "  $latency"
echo "  $timehist"

sudo rm -f "$data"
