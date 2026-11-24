#!/usr/bin/env bash
# usage: bash tools/lab.sh <step>
#   0      준비      emmcsim을 띄우고 4 KB read 한 번의 시간을 장치 내부와 fio에서 비교한다 (nbd 경로 비용)
#   1a     실습 1-1  die 1개: 4 KB random read, QD 1 / 4 / 32
#   1b     실습 1-2  die 2개: 같은 측정
#   1c     실습 1-3  die 4개: 같은 측정
#   1d     실습 1-4  1 MB sequential write: die×plane = 1×1, 1×2, 2×2
#   2a     실습 2-1  logger A: 200 B line마다 write + fsync (blkio.bt로 FLUSH와 FUA를 측정)
#   2b     실습 2-2  logger B: 64 KB씩 모아서 write + fsync
#   2c     실습 2-3  전원 차단: fsync하는 logger와 fsync하지 않는 logger, 데이터 손실 여부 확인
#   3a     실습 3-1  UI read만 (기준)
#   3b     실습 3-2  OTA write 256 MB를 20 MB/s로 제한, suspension 없음
#   3c     실습 3-3  같은 부하, suspend=ips
#   3d     실습 3-4  같은 부하, suspend=ipc
#   down   emmcsim을 내린다
set -u
cd "$(dirname "$0")/.." || exit 1
step=${1:-}

if [ -z "$step" ]; then
    sed -n '/^# usage:/,/^#   down/p' "$0"
    exit 0
fi
if [ "$(id -u)" -ne 0 ]; then
    make -s || exit 1                                # 빌드는 일반 사용자로 한다
    exec sudo TS="${TS:-}" bash "$0" "$step"
fi
mkdir -p logs
OWNER="${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}"
trap 'chown -R "$OWNER" logs 2>/dev/null' EXIT
export EMMC_CTL=/tmp/emmcsim.ctl
MNT=/mnt/emmc
SAY_COUNT=0

say() {
    if [ "$SAY_COUNT" -gt 0 ]; then
        printf '\n'
    fi
    printf '\033[1m== %s\033[0m\n' "$*"
    SAY_COUNT=$((SAY_COUNT + 1))
}

show() {
    printf '$'
    printf ' %q' "$@"
    printf '\n'
}

dev_up() {
    local args=("$@")
    if [ -n "${TS:-}" ]; then args+=("time_scale=$TS"); fi
    bash tools/emmc.sh up "${args[@]}" || exit 1
}

dev_mkfs() {
    show bash tools/emmc.sh mkfs
    bash tools/emmc.sh mkfs >/dev/null || exit 1
}

reset() {
    ./bin/emmcctl reset >/dev/null
}

stat_save() {
    ./bin/emmcctl stat > "$1"
    ./bin/emmcctl stat raw > "${1%.txt}.raw"
}

raw() { # raw <file.raw> <key>
    grep "^$2=" "$1" | cut -d= -f2
}

show_stat() { # show_stat <stat file>
    local file=$1
    awk '
        /^die[0-9]+[[:space:]]+busy/ {
            printf "  %s: busy=%s, read=%s, program=%s, erase=%s, redo=%s (%s ms), suspend=%s\n", \
                   $1, $5, $7, $9, $11, $13, $15, $18
        }
        /^bus[[:space:]]+busy/ {
            printf "  bus: busy=%s, NAND channel=%s\n", $5, $9
        }
        /^SRAM[[:space:]]/ {
            printf "  SRAM free-slot wait: %s %s\n", $8, $9
        }
    ' "$file"
}

show_sync_stat() { # show_sync_stat <stat file>
    local file=$1
    awk '
        /^host[[:space:]]/ {
            printf "  host: write=%s MB, FLUSH=%s, FUA=%s\n", $10, $13, $15
        }
        /^nand[[:space:]]/ {
            printf "  NAND: program=%s MB (%s pages), padding=%s MB, erases=%s\n", \
                   $16, $13, $19, $22
        }
        /NAND bytes \/ host bytes written/ {
            sub(/.*= /, "")
            print "  write amplification: " $0
        }
        /^flush[[:space:]]+latency inside the device:/ {
            sub(/^flush[[:space:]]+latency inside the device:/, "device FLUSH latency:")
            print "  " $0
        }
    ' "$file"
}

show_blkio_summary() { # show_blkio_summary <bpftrace output>
    awk '
        /^@requests\[FF/ { flush += $NF }
        /^@requests\[WF/ { fua += $NF }
        END { printf "block requests: FLUSH=%d, FUA writes=%d\n", flush, fua }
    ' "$1"
}

fio_raw() { # fio_raw <log name> <fio args...>
    local name=$1
    shift
    local has_size=0
    for arg in "$@"; do
        case "$arg" in
            --size=*) has_size=1 ;;
        esac
    done
    local args=(
        --name="${name##*-}"
        --filename=/dev/nbd0
        --direct=1
        --ioengine=libaio
    )
    if [ "$has_size" -eq 0 ]; then
        args+=(--size=512M)
    fi
    args+=("$@")
    show fio "${args[@]}" --output-format=json --output="logs/$name.json"
    fio "${args[@]}" --output-format=json --output="logs/$name.json" >/dev/null 2>&1
    python3 tools/fiosum.py "logs/$name.json"
}

# ---------- 준비 ----------
step0() {
    say "기본 설정 (conf/tv.conf)"
    dev_up
    show ./bin/emmcctl conf
    ./bin/emmcctl conf \
        | tee logs/lab0-conf.txt \
        | grep -E '^(dies|planes|page_kb|t_r_us|t_prog_us|host_mbps|time_scale)='
    say "4 KB random read, QD 1, 5초: fio가 본 시간과 장치 안의 시간"
    reset
    fio_raw lab0-qd1 --rw=randread --bs=4k --iodepth=1 --runtime=5 --time_based
    stat_save logs/lab0-stat.txt
    show_stat logs/lab0-stat.txt
    grep 'read   latency' logs/lab0-stat.txt | sed 's/^/  /'
}

# ---------- 실습 1 ----------
read_qd() { # read_qd <dies>
    local d=$1
    dev_up dies="$d"
    for qd in 1 4 32; do
        say "die ${d}개, QD $qd: 4 KB random read 3초"
        reset
        fio_raw "lab1-d$d-qd$qd" \
            --rw=randread --bs=4k --iodepth="$qd" --runtime=3 --time_based
        stat_save "logs/lab1-d$d-qd$qd-stat.txt"
        show_stat "logs/lab1-d$d-qd$qd-stat.txt"
    done
}
step1a() {
    read_qd 1
}

step1b() {
    read_qd 2
}

step1c() {
    read_qd 4
}

step1d() {
    for config in "1 1" "1 2" "2 2"; do
        set -- $config
        say "die $1개 x plane $2개: 1 MB sequential write 64 MB (QD 4)"
        dev_up dies="$1" planes="$2"
        reset
        fio_raw "lab1-w-d$1p$2" --rw=write --bs=1M --iodepth=4 --size=64M
        stat_save "logs/lab1-w-d$1p$2-stat.txt"
        show_stat "logs/lab1-w-d$1p$2-stat.txt"
    done
}

# ---------- 실습 2 ----------
per_line() { # per_line <raw stat> <lines>
    local hb nb pad fl
    hb=$(raw "$1" bytes_written)
    nb=$(raw "$1" nand_bytes)
    pad=$(raw "$1" pad_bytes)
    fl=$(raw "$1" host_flushes)
    awk \
        -v hb="$hb" -v nb="$nb" -v pad="$pad" -v fl="$fl" -v n="$2" \
        'BEGIN {
            printf "  line 하나당: host write %.0f B, NAND program %.0f B " \
                   "(그중 padding %.0f B), FLUSH %.3f번\n", \
                   hb / n, nb / n, pad / n, fl / n
        }'
}

step2a() {
    say "logger A: 200 B line 1000개, line마다 write + fsync"
    dev_up
    dev_mkfs
    reset
    show bpftrace tools/blkio.bt -c "./bin/logger -f $MNT/a.log -m line -n 1000"
    bpftrace tools/blkio.bt \
        -c "./bin/logger -f $MNT/a.log -m line -n 1000" \
        > logs/lab2-a-blkio.txt 2>&1
    grep -E '^logger result:|^sync latency' logs/lab2-a-blkio.txt
    stat_save logs/lab2-a-stat.txt
    show_stat logs/lab2-a-stat.txt
    show_sync_stat logs/lab2-a-stat.txt
    per_line logs/lab2-a-stat.raw 1000
    show_blkio_summary logs/lab2-a-blkio.txt
}

step2b() {
    say "logger B: 200 B line 10000개, 64 KB씩 모아서 write + fsync"
    dev_up
    dev_mkfs
    reset
    show bpftrace tools/blkio.bt -c "./bin/logger -f $MNT/b.log -m batch -b 64 -n 10000"
    bpftrace tools/blkio.bt \
        -c "./bin/logger -f $MNT/b.log -m batch -b 64 -n 10000" \
        > logs/lab2-b-blkio.txt 2>&1
    grep -E '^logger result:|^sync latency' logs/lab2-b-blkio.txt
    stat_save logs/lab2-b-stat.txt
    show_stat logs/lab2-b-stat.txt
    show_sync_stat logs/lab2-b-stat.txt
    per_line logs/lab2-b-stat.raw 10000
    show_blkio_summary logs/lab2-b-blkio.txt
}

powercut_run() { # powercut_run <name> <logger args...>
    local name=$1
    shift
    rm -f "logs/lab2-$name-ack.txt"
    rm -f "logs/lab2-$name-write.txt"
    show ./bin/logger -f "$MNT/$name.log" "$@" \
        -a "logs/lab2-$name-ack.txt" -w "logs/lab2-$name-write.txt" '&'
    ./bin/logger -f "$MNT/$name.log" "$@" \
        -a "logs/lab2-$name-ack.txt" \
        -w "logs/lab2-$name-write.txt" \
        > "logs/lab2-$name-logger.txt" 2>&1 &
    local pid=$!
    sleep 2
    show bash tools/emmc.sh powercut
    bash tools/emmc.sh powercut || exit 1
    wait "$pid" 2>/dev/null
    show bash tools/emmc.sh restore
    bash tools/emmc.sh restore > /dev/null || exit 1
    show ./bin/logger -c "$MNT/$name.log" \
        -a "logs/lab2-$name-ack.txt" -w "logs/lab2-$name-write.txt"
    ./bin/logger -c "$MNT/$name.log" \
        -a "logs/lab2-$name-ack.txt" \
        -w "logs/lab2-$name-write.txt" \
        | tee "logs/lab2-$name-check.txt"
}

step2c() {
    dev_up
    dev_mkfs
    say "batch logger (16 KB마다 fsync, 초당 5000 line): 2초 뒤 전원 차단"
    show ./bin/logger -f "$MNT/sync.log" -P
    ./bin/logger -f "$MNT/sync.log" -P >/dev/null
    powercut_run sync -m batch -b 16 -r 5000 -n 10000000
    say "빈 파일의 이름을 먼저 durable하게 만든 뒤, fsync하지 않고 append: 2초 뒤 전원 차단"
    show ./bin/logger -f "$MNT/nosync.log" -P
    ./bin/logger -f "$MNT/nosync.log" -P >/dev/null
    powercut_run nosync -A -m nosync -r 5000 -n 10000000
}

# ---------- 실습 3 ----------
ota_setup() { # ota_setup <emmcsim keys...>
    dev_up "$@"
    dev_mkfs
    show dd if=/dev/urandom "of=$MNT/ui.bin" bs=1M count=64 conv=fsync status=none
    dd if=/dev/urandom of="$MNT/ui.bin" bs=1M count=64 conv=fsync status=none
    show sync
    sync
    show "echo 3 > /proc/sys/vm/drop_caches"
    echo 3 > /proc/sys/vm/drop_caches # UI read를 장치에서 읽도록 page cache를 비운다
    reset
}

UI_WINDOW_S=4
UI_FIO=(
    --name=ui
    --filename="$MNT/ui.bin"
    --direct=1
    --rw=randread
    --bs=4k
    --iodepth=1
    --time_based
    --runtime="$UI_WINDOW_S"
)

OTA_CGROUP=/sys/fs/cgroup/lab11ota
OTA_WBPS=20000000 # 20 MB/s

setup_ota_limit() {
    local dev
    if ! grep -qw io /sys/fs/cgroup/cgroup.controllers || \
       ! grep -qw memory /sys/fs/cgroup/cgroup.controllers; then
        echo "cgroup v2 memory/io controller가 없다 (check_env.sh 참고)"
        return 1
    fi
    if [ -e "$OTA_CGROUP" ] && ! rmdir "$OTA_CGROUP" 2>/dev/null; then
        echo "$OTA_CGROUP 이 비어 있지 않다. 이전 실행의 cgroup을 확인한다"
        return 1
    fi
    show "echo '+memory +io' > /sys/fs/cgroup/cgroup.subtree_control"
    if ! echo '+memory +io' > /sys/fs/cgroup/cgroup.subtree_control; then
        echo "cgroup v2 memory/io controller를 활성화할 수 없다"
        return 1
    fi
    mkdir "$OTA_CGROUP" || return 1
    dev=$(cat /sys/block/nbd0/dev 2>/dev/null)
    if [ -z "$dev" ]; then
        rmdir "$OTA_CGROUP" 2>/dev/null
        echo "/dev/nbd0의 major:minor를 읽을 수 없다"
        return 1
    fi
    show "echo \"$dev wbps=$OTA_WBPS\" > $OTA_CGROUP/io.max"
    if ! echo "$dev wbps=$OTA_WBPS" > "$OTA_CGROUP/io.max"; then
        rmdir "$OTA_CGROUP" 2>/dev/null
        echo "OTA write limit을 설정할 수 없다"
        return 1
    fi
}

cleanup_ota_limit() {
    rmdir "$OTA_CGROUP" 2>/dev/null || true
}

ui_with_ota() { # ui_with_ota <name>
    local name=$1
    setup_ota_limit || return 1
    show sh -c 'echo $$ > "$1/cgroup.procs"; shift; exec "$@"' sh "$OTA_CGROUP" ./bin/otawriter -f "$MNT/ota.bin" -s 256
    sh -c 'echo $$ > "$1/cgroup.procs"; shift; exec "$@"' sh "$OTA_CGROUP" ./bin/otawriter -f "$MNT/ota.bin" -s 256 \
        > "logs/lab3-$name-ota.txt" 2>&1 &
    local opid=$!
    # Start fio immediately after OTA so all comparisons use the same
    # fixed window beginning with the concurrent workload.
    say "fio UI latency: application-to-device time, including ext4 and NBD path"
    show fio "${UI_FIO[@]}" --output-format=json --output="logs/lab3-$name-ui.json"
    fio "${UI_FIO[@]}" \
        --output-format=json \
        --output="logs/lab3-$name-ui.json" \
        >/dev/null 2>&1 &
    local fpid=$!
    wait "$fpid" 2>/dev/null
    # Snapshot counters and trace at the end of the fixed UI measurement window,
    # before the OTA tail can extend the device-stat sample.
    stat_save "logs/lab3-$name-stat.txt"
    ./bin/emmcctl trace > "logs/lab3-$name-trace.csv"
    python3 tools/fiosum.py "logs/lab3-$name-ui.json"
    say "emmcsim internal counters during the fixed UI measurement window"
    show_stat "logs/lab3-$name-stat.txt"
    grep 'read   latency' "logs/lab3-$name-stat.txt" | sed 's/^/  /'
    wait "$opid" 2>/dev/null
    cleanup_ota_limit
    grep '^OTA result:' "logs/lab3-$name-ota.txt"
}

step3a() {
    say "UI read만: 4 KB random read, QD 1, ${UI_WINDOW_S}초"
    ota_setup
    say "fio UI latency: application-to-device time, including ext4 and NBD path"
    show fio "${UI_FIO[@]}" --output-format=json --output=logs/lab3-a-ui.json
    fio "${UI_FIO[@]}" --output-format=json --output=logs/lab3-a-ui.json >/dev/null 2>&1
    python3 tools/fiosum.py logs/lab3-a-ui.json
    stat_save logs/lab3-a-stat.txt
    show_stat logs/lab3-a-stat.txt
    grep 'read   latency' logs/lab3-a-stat.txt | sed 's/^/  /'
}

step3b() {
    say "OTA 256 MB + UI read, max 20 MB/s, suspend=off"
    ota_setup
    ui_with_ota b
}

step3c() {
    say "OTA 256 MB + UI read, max 20 MB/s, suspend=ips"
    ota_setup suspend=ips
    ui_with_ota c
}

step3d() {
    say "OTA 256 MB + UI read, max 20 MB/s, suspend=ipc"
    ota_setup suspend=ipc
    ui_with_ota d
}

stepdown() {
    show bash tools/emmc.sh down
    bash tools/emmc.sh down
}

case "$step" in
    0|1a|1b|1c|1d|2a|2b|2c|3a|3b|3c|3d|down)
        "step$step"
        ;;
    *)
        echo "알 수 없는 단계: $step"
        sed -n '/^# usage:/,/^#   down/p' "$0"
        exit 2
        ;;
esac
