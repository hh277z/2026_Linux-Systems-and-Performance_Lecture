#!/usr/bin/env bash
# emmc.sh: start, stop and power-cut the emmcsim device. Run as root.
#
#   emmc.sh up [key=value ...]   start emmcsim with conf/tv.conf (keys override it), attach /dev/nbd0
#   emmc.sh mkfs                 ext4 on /dev/nbd0, mounted on /mnt/emmc
#   emmc.sh down                 unmount, detach, stop emmcsim
#   emmc.sh powercut             cut the power now: what is not on NAND is lost
#   emmc.sh restore              start again from the NAND image, mount (ext4 replays its journal)
#   emmc.sh status
set -u
LAB=$(cd "$(dirname "$0")/.." && pwd)
SOCK=/tmp/emmcsim.sock
CTL=/tmp/emmcsim.ctl
PIDF=/tmp/emmcsim.pid
IMG=/tmp/emmcsim.img
KEYS=/tmp/emmcsim.keys
DEV=/dev/nbd0
MNT=/mnt/emmc
export EMMC_CTL=$CTL

[ "$(id -u)" -eq 0 ] || {
    echo "emmc.sh: root 권한이 필요합니다"
    exit 1
}

running() {
    [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null
}

stop_nbdkit() {
    if running; then
        local pid
        pid=$(cat "$PIDF")
        kill "$pid" 2>/dev/null
        for _ in $(seq 50); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$pid" 2>/dev/null
    fi
    rm -f "$PIDF" "$SOCK"
}

detach() {
    if mountpoint -q "$MNT"; then
        umount "$MNT" 2>/dev/null || umount -l "$MNT"
    fi
    nbd-client -c "$DEV" >/dev/null 2>&1 && nbd-client -d "$DEV" >/dev/null 2>&1
    stop_nbdkit
}

start() { # start <key=value ...>
    modprobe nbd 2>/dev/null
    if [ ! -b "$DEV" ]; then
        echo "emmc.sh: $DEV 장치를 찾을 수 없습니다. WSL의 nbd 설정을 확인하세요."
        exit 1
    fi
    detach
    rm -f "$CTL"
    nbdkit -f -U "$SOCK" --threads 32 \
        "$LAB/bin/emmcsim.so" \
        conf="$LAB/conf/tv.conf" \
        ctl="$CTL" save="$IMG" "$@" \
        </dev/null >/tmp/emmcsim.log 2>&1 &
    echo $! > "$PIDF"
    for _ in $(seq 100); do
        if [ -S "$SOCK" ]; then
            break
        fi
        sleep 0.05
    done
    if [ ! -S "$SOCK" ]; then
        echo "emmc.sh: emmcsim did not start:"
        cat /tmp/emmcsim.log
        exit 1
    fi
    if ! nbd-client -unix "$SOCK" "$DEV" -b 4096 >/dev/null 2>&1; then
        echo "emmc.sh: nbd-client failed"
        exit 1
    fi
    for _ in $(seq 50); do
        if [ "$(cat /sys/block/nbd0/size 2>/dev/null)" != 0 ]; then
            break
        fi
        sleep 0.05
    done
}

case "${1:-}" in
up)
    shift
    printf '%s\n' "$@" > "$KEYS"
    start "$@"
    rm -f "$IMG"
    ;;
mkfs)
    mkfs.ext4 -q -F -E nodiscard,lazy_itable_init=0,lazy_journal_init=0 "$DEV" || exit 1
    mkdir -p "$MNT"
    mount -o noatime "$DEV" "$MNT" && chmod 1777 "$MNT"
    ;;
down)
    detach
    ;;
powercut)
    if ! running; then
        echo "emmc.sh: emmcsim is not running"
        exit 1
    fi
    "$LAB/bin/emmcctl" powercut
    detach 2>/dev/null           # the device is gone: unmount lazily, detach, stop
    ;;
restore)
    if [ ! -f "$IMG" ]; then
        echo "emmc.sh: no image $IMG (run powercut first)"
        exit 1
    fi
    keys=()
    while IFS= read -r k; do
        if [ -n "$k" ]; then
            keys+=("$k")
        fi
    done < "$KEYS"
    start "${keys[@]}" image="$IMG"
    mkdir -p "$MNT"
    mount -o noatime "$DEV" "$MNT" && chmod 1777 "$MNT"
    ;;
status)
    if running; then
        echo "emmcsim running (pid $(cat "$PIDF"))"
    else
        echo "emmcsim not running"
    fi
    if nbd-client -c "$DEV" >/dev/null 2>&1; then
        echo "$DEV attached"
    else
        echo "$DEV not attached"
    fi
    if mountpoint -q "$MNT"; then
        echo "$MNT mounted"
    else
        echo "$MNT not mounted"
    fi
    ;;
*)
    sed -n '2,10p' "$0"
    exit 2
    ;;
esac
