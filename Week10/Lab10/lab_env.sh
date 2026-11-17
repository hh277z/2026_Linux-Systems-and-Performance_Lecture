#!/usr/bin/env bash
# Only prepare/remove cgroups. Observation is performed manually in terminal B.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
[[ $EUID -eq 0 ]] || { echo 'Use sudo bash lab_env.sh setup A|B|C or cleanup' >&2; exit 2; }
mkdir -p state
exec 9>state/setup.lock
flock -n 9 || { echo 'Another setup/cleanup is running.' >&2; exit 2; }
envfile=state/session.env

remove_group() {
    local group=$1
    [[ $group =~ ^/sys/fs/cgroup/week10-manual-[0-9]+-[0-9]+$ ]] || { echo 'Unexpected group path.' >&2; return 1; }
    if [[ -d $group ]]; then
        echo 1 > "$group/cgroup.kill"
        for ((attempt=0; attempt<50; attempt++)); do
            if grep -q '^populated 0$' "$group/cgroup.events"; then break; fi
            sleep .1
        done
        for name in media existing new; do
            if [[ -d $group/$name ]]; then rmdir -- "$group/$name"; fi
        done
        rmdir -- "$group"
    fi
}

case ${1:-} in
setup)
    scenario=${2:-}
    [[ $scenario =~ ^[ABC]$ ]] || { echo 'Choose A, B, or C.' >&2; exit 2; }
    if [[ -e $envfile ]]; then
        echo 'Run cleanup before creating a new session.' >&2; exit 2
    fi
    grep -qw memory /sys/fs/cgroup/cgroup.subtree_control
    available=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)
    [[ $available -ge 1048576 ]] || { echo 'Need at least 1 GiB MemAvailable.' >&2; exit 2; }
    CG=/sys/fs/cgroup/week10-manual-$(date +%s)-$$
    mkdir -- "$CG"
    trap 'remove_group "$CG"' ERR
    [[ -e $CG/cgroup.kill && -e $CG/memory.peak ]]
    case $scenario in A) limit=268435456;; B) limit=335544320;; C) limit=201326592;; esac
    echo "$limit" > "$CG/memory.max"
    echo 0 > "$CG/memory.swap.max"
    echo +memory > "$CG/cgroup.subtree_control"
    if [[ $scenario == C ]]; then
        mkdir -- "$CG/existing" "$CG/new"
    else
        mkdir -- "$CG/media"
    fi
    printf "CG='%s'\nSCENARIO='%s'\n" "$CG" "$scenario" > "$envfile"
    chmod 644 "$envfile"
    trap - ERR
    echo "Prepared $scenario. Both terminals: source state/session.env"
    echo "CG=$CG"
    ;;
cleanup)
    if [[ ! -e $envfile ]]; then echo 'No active session.'; exit 0; fi
    CG=$(sed -n "s/^CG='\([^']*\)'$/\1/p" "$envfile")
    remove_group "$CG"
    rm -- "$envfile"
    echo "Removed $CG"
    ;;
*) echo 'Usage: sudo bash lab_env.sh setup A|B|C | cleanup' >&2; exit 2;;
esac
