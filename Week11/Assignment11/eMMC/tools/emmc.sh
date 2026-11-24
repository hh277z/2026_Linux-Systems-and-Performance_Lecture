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

[ "$(id -u)" -eq 0 ] || { echo "emmc.sh: run as root (sudo)"; exit 1; }
show() { echo "\$ $*"; }

running() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }

stop_nbdkit() {
    if running; then
        local pid
        pid=$(cat "$PIDF")
        kill "$pid" 2>/dev/null
        for _ in $(seq 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill -9 "$pid" 2>/dev/null
    fi
    rm -f "$PIDF" "$SOCK"
}

detach() {
    mountpoint -q "$MNT" && { umount "$MNT" 2>/dev/null || umount -l "$MNT"; }
    nbd-client -c "$DEV" >/dev/null 2>&1 && nbd-client -d "$DEV" >/dev/null 2>&1
    stop_nbdkit
}

start() {    # start <key=value ...>
    modprobe nbd 2>/dev/null
    [ -b "$DEV" ] || { echo "emmc.sh: $DEV not found (sudo modprobe nbd)"; exit 1; }
    detach
    rm -f "$CTL"
    show nbdkit -f -U $SOCK --threads 32 bin/emmcsim.so conf=conf/tv.conf "$@"
    nbdkit -f -U "$SOCK" --threads 32 \
        "$LAB/bin/emmcsim.so" \
        conf="$LAB/conf/tv.conf" \
        ctl="$CTL" save="$IMG" "$@" \
        </dev/null >/tmp/emmcsim.log 2>&1 &
    echo $! > "$PIDF"
    for _ in $(seq 100); do [ -S "$SOCK" ] && break; sleep 0.05; done
    [ -S "$SOCK" ] || {
        echo "emmc.sh: emmcsim did not start:"
        cat /tmp/emmcsim.log
        exit 1
    }
    show nbd-client -unix $SOCK $DEV -b 4096
    nbd-client -unix "$SOCK" "$DEV" -b 4096 >/dev/null 2>&1 || {
        echo "emmc.sh: nbd-client failed"
        exit 1
    }
    for _ in $(seq 50); do [ "$(cat /sys/block/nbd0/size 2>/dev/null)" != 0 ] && break; sleep 0.05; done
}

case "${1:-}" in
up)
    shift
    printf '%s\n' "$@" > "$KEYS"
    start "$@"
    rm -f "$IMG"
    ;;
mkfs)
    show mkfs.ext4 -q -F -E nodiscard,lazy_itable_init=0,lazy_journal_init=0 $DEV
    mkfs.ext4 -q -F -E nodiscard,lazy_itable_init=0,lazy_journal_init=0 "$DEV" || exit 1
    mkdir -p "$MNT"
    show mount -o noatime $DEV $MNT
    mount -o noatime "$DEV" "$MNT" && chmod 1777 "$MNT"
    ;;
down)
    detach
    ;;
powercut)
    running || { echo "emmc.sh: emmcsim is not running"; exit 1; }
    show bin/emmcctl powercut
    "$LAB/bin/emmcctl" powercut
    detach 2>/dev/null           # the device is gone: unmount lazily, detach, stop
    ;;
restore)
    [ -f "$IMG" ] || { echo "emmc.sh: no image $IMG (run powercut first)"; exit 1; }
    keys=()
    while IFS= read -r k; do [ -n "$k" ] && keys+=("$k"); done < "$KEYS"
    start "${keys[@]}" image="$IMG"
    mkdir -p "$MNT"
    show mount -o noatime $DEV $MNT
    mount -o noatime "$DEV" "$MNT" && chmod 1777 "$MNT"
    ;;
status)
    running && echo "emmcsim running (pid $(cat "$PIDF"))" || echo "emmcsim not running"
    nbd-client -c "$DEV" >/dev/null 2>&1 && echo "$DEV attached" || echo "$DEV not attached"
    mountpoint -q "$MNT" && echo "$MNT mounted" || echo "$MNT not mounted"
    ;;
*)
    sed -n '2,10p' "$0"
    exit 2
    ;;
esac
